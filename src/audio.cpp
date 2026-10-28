#include "audio.h"

#include <Arduino.h>
#include <Preferences.h>
#include <driver/i2s.h>
#include <math.h>

#include "clock.h"
#include "es8311.h"
#include "storage.h"

static const int I2S_MCK_PIN = 13;
static const int I2S_BCK_PIN = 14;
static const int I2S_WS_PIN = 47;
static const int I2S_DOUT_PIN = 48; // to codec DAC (speaker)
static const int I2S_DIN_PIN = 21;  // from codec ADC (mic)
static const int PA_ENABLE_PIN = 39; // speaker amplifier

static const int MCLK_MULTIPLE = 256;
static const int SPEAKER_VOLUME = 80; // 0-100
// Mic gain: the analog preamp (0-30 dB in 3 dB steps), then digital scaling
// after the ADC (6 dB steps). Only the preamp changes what clips.
static const int MAX_GAIN_DB = 54;
static const int DEFAULT_AUTO_GAIN_DB = 36;
// Auto gain while recording: down a step at once when peaks near clipping,
// up a step after a stretch without anything above AGC_QUIET
static const int AGC_LOUD = 23000;  // about -3 dBFS
static const int AGC_QUIET = 3300;  // about -20 dBFS
static const int AGC_RELEASE_CHUNKS = 90; // ~1.5 s
// Playback boosts each moment so its loudest moment is near full scale, up to this much
static const float MAX_PLAYBACK_GAIN = 8.0f;

static const size_t MIN_MOMENT_SAMPLES = SAMPLE_RATE / 4; // anything shorter is an accidental tap
static const size_t PSRAM_RESERVE = 256 * 1024;         // left free for the rest of the app
static const uint64_t SD_RESERVE = 1024 * 1024;         // for file system overhead
static const int CHUNK_FRAMES = SAMPLE_RATE / 1000 * 16; // 16 ms
static const size_t PLAY_BUFFER_SAMPLES = 16 * CHUNK_FRAMES; // read from the SD card 256 ms at a time
static const uint32_t STANDBY_AFTER_MS = 2000;               // idle time before the codec powers down
// Varispeed, per 16 ms chunk: holding a key multiplies or divides the speed by
// SPEED_STEP (so it keeps accelerating, like a tape reel), and on release it
// eases back to normal
static const float SPEED_STEP = 1.04f;   // 2x after ~0.3 s held; 1x to -2x in ~1.6 s
static const float SPEED_RETURN = 0.85f; // fraction of the distance to 1x left after each chunk (~0.1 s)
static const float MAX_SPEED = 2.0f;
// Below this the speed stops shrinking by steps and slides straight through
// zero: the tape stops, then runs backwards (and on up to -2x while held)
static const float SCRUB_SPEED = 0.25f;
static const float SCRUB_STEP = 0.05f; // per chunk: 0.16 s from +0.25 to -0.25

enum CommandType : uint8_t {
    CMD_RECORD_START,
    CMD_RECORD_STOP,
    CMD_PLAY,
    CMD_STOP_PLAYBACK,
    CMD_DELETE,
    CMD_SELF_TEST,
    CMD_SUSPEND,
    CMD_TAG,
    CMD_FORMAT,
    CMD_GAIN,
};

struct Command {
    CommandType type;
    int arg;
    int arg2;
};

static QueueHandle_t commandQueue;

// The moment list is only modified by the audio task; the lock keeps readers on
// other tasks from seeing it half-updated
static SemaphoreHandle_t momentLock;
static Moment *moments = nullptr; // MAX_MOMENTS entries, in PSRAM
static volatile int momentTotal = 0;
static volatile uint32_t listVersion = 0;
static int nextMomentNumber = 1;

// Recording goes into a buffer sized for the longest moment that fits, which is
// then shrunk to the moment (in PSRAM) or written to the card and freed.
// At 48 kHz a minute is 5.8 MB, too much to keep reserved.
static int16_t *scratch = nullptr;
static volatile size_t scratchLength = 0; // samples taken in so far
// With a card in, the samples go straight onto it (see storage.h); the list
// waveform and gain then come from a running envelope of chunk peaks
static bool streamingRecord = false;
static Moment streamMoment;
static const size_t ENVELOPE_MAX = 450000; // entries (900 KB): one per 16 ms chunk up to 2 h, coarser beyond
static uint16_t *envelope = nullptr;
static size_t envelopeChunks = 1; // chunks per entry
static volatile size_t recordLimit = 0;
static time_t recordStartedAt = 0;
static uint16_t recordStartedMs = 0;

static volatile AudioState state = AUDIO_IDLE;
static volatile int playingIndex = -1;
static volatile double playPos = 0; // in samples; fractional with varispeed
static volatile float speed = 1;
static volatile int speedDirection = 0;
static volatile int formatResult = 0;
static volatile int gainSetting = GAIN_AUTO; // dB, or GAIN_AUTO
static volatile int gainNow = DEFAULT_AUTO_GAIN_DB; // what the codec is set to
static int agcQuietChunks = 0;
static int agcChanges = 0;

// Playback of moments on the SD card streams through this buffer
static File playFile;
static int16_t playBuffer[PLAY_BUFFER_SAMPLES];
static size_t playBufferStart = 0, playBufferLength = 0; // in samples of the moment
static bool playFileEnded = false;

static es8311_handle_t codec = nullptr;
static SemaphoreHandle_t suspendDone;
// Codec powered and I2S streaming. Off while idle: the codec, mic and I2S DMA
// draw power, and the task would otherwise wake every 16 ms.
static volatile bool running = false;
static uint32_t idleSince = 0;
// After power-up the ADC gives ~70 ms of silence with a click, then settles
static const size_t SETTLE_SAMPLES = SAMPLE_RATE / 10;
static size_t settleLeft = 0; // samples still to drop from the start of a recording

static bool codecInit();

// Nearest gain the codec can do: 3 dB steps up to 30, then 6 dB steps
static int gainStepUp(int db) { return db < 30 ? db + 3 : db + 6; }
static int gainStepDown(int db) { return db > 30 ? db - 6 : db - 3; }

static void applyGain(int db) {
    db = constrain(db, 0, MAX_GAIN_DB);
    int pga = min(db, 30) / 3;
    int scale = (db - pga * 3) / 6;
    if (codec) {
        es8311_microphone_pga_set(codec, pga);
        es8311_microphone_gain_set(codec, (es8311_mic_gain_t)scale);
    }
    gainNow = pga * 3 + scale * 6;
}

// Per 16 ms chunk while recording with auto gain
static void autoGain(const int16_t *in, size_t frames, size_t skip) {
    int peak = 0;
    for (size_t i = skip; i < frames; i++) peak = max(peak, abs((int)in[2 * i]));
    if (peak > AGC_LOUD && gainNow > 0) {
        applyGain(gainStepDown(gainNow));
        agcQuietChunks = 0;
        agcChanges++;
    } else if (peak < AGC_QUIET) {
        if (++agcQuietChunks >= AGC_RELEASE_CHUNKS && gainNow < MAX_GAIN_DB) {
            applyGain(gainStepUp(gainNow));
            agcQuietChunks = 0;
            agcChanges++;
        }
    } else {
        agcQuietChunks = 0;
    }
}

static void audioWake() {
    if (running) return;
    uint32_t start = millis();
    i2s_zero_dma_buffer(I2S_NUM_0);
    i2s_start(I2S_NUM_0);
    if (!codecInit()) Serial.println("ES8311 codec init failed");
    running = true;
    settleLeft = SETTLE_SAMPLES;
    Serial.printf("[audio] codec on in %lu ms\n", millis() - start);
}

static void audioStandby() {
    if (!running) return;
    digitalWrite(PA_ENABLE_PIN, LOW);
    if (codec) es8311_power_down(codec);
    i2s_stop(I2S_NUM_0); // i2s_read() would now block, so the task waits on commands instead
    running = false;
    Serial.println("[audio] codec off");
}

static uint64_t samplesLeft() {
    if (momentTotal >= MAX_MOMENTS) return 0;
    size_t ramBytes = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    uint64_t ramSamples = ramBytes > PSRAM_RESERVE ? (ramBytes - PSRAM_RESERVE) / sizeof(int16_t) : 0;
    if (!storageAvailable()) return ramSamples;
    uint64_t freeBytes = storageFreeBytes();
    return freeBytes > SD_RESERVE ? (freeBytes - SD_RESERVE) / sizeof(int16_t) : 0;
}

// Room for one recording: it's buffered in PSRAM, and has to fit on the card too
static size_t recordableSamples() {
    if (storageAvailable()) return (size_t)min((uint64_t)MAX_RECORDING_SECONDS * SAMPLE_RATE, samplesLeft());
    size_t ramBytes = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t ram = ramBytes > PSRAM_RESERVE ? (ramBytes - PSRAM_RESERVE) / sizeof(int16_t) : 0;
    return (size_t)min((uint64_t)ram, samplesLeft());
}

static void startRecording(uint32_t seconds) {
    recordLimit = min((size_t)seconds * SAMPLE_RATE, recordableSamples());
    if (recordLimit < MIN_MOMENT_SAMPLES) {
        Serial.println("No room for another recording");
        return;
    }
    streamingRecord = storageAvailable();
    if (streamingRecord) {
        size_t chunks = recordLimit / CHUNK_FRAMES + 1;
        envelopeChunks = (chunks + ENVELOPE_MAX - 1) / ENVELOPE_MAX;
        free(envelope);
        envelope = (uint16_t *)ps_calloc(chunks / envelopeChunks + 1, sizeof(uint16_t));
        if (!envelope) {
            Serial.println("Can't allocate the recording envelope");
            return;
        }
    } else {
        scratch = (int16_t *)ps_malloc(recordLimit * sizeof(int16_t));
        if (!scratch) {
            Serial.println("Can't allocate the recording buffer");
            return;
        }
    }
    scratchLength = 0;
    agcQuietChunks = agcChanges = 0;
    recordStartedAt = 0; // set when the first sample arrives
    recordStartedMs = 0;
    state = AUDIO_RECORDING;
    Serial.printf("Recording started at %lu ms\n", millis());
}

// Playback gain and list waveform
static void analyse(Moment &moment, const int16_t *samples) {
    size_t length = moment.length;
    int peak = 1;
    for (size_t i = 0; i < length; i++) peak = max(peak, abs((int)samples[i]));
    moment.gain = min(MAX_PLAYBACK_GAIN, 29000.0f / peak);
    for (int b = 0; b < WAVEFORM_BINS; b++) {
        size_t from = length * b / WAVEFORM_BINS, to = length * (b + 1) / WAVEFORM_BINS;
        int binPeak = 0;
        for (size_t i = from; i < to; i++) binPeak = max(binPeak, abs((int)samples[i]));
        // sqrt keeps quiet passages visible next to loud ones
        moment.waveform[b] = (uint8_t)(255 * sqrtf((float)binPeak / peak));
    }
}

// Playback gain and list waveform from the envelope of a streamed recording
static void analyseEnvelope(Moment &moment, size_t samples) {
    size_t used = max((size_t)1, (samples / CHUNK_FRAMES + envelopeChunks - 1) / envelopeChunks);
    int peak = 1;
    for (size_t i = 0; i < used; i++) peak = max(peak, (int)envelope[i]);
    moment.gain = min(MAX_PLAYBACK_GAIN, 29000.0f / peak);
    for (int b = 0; b < WAVEFORM_BINS; b++) {
        size_t from = used * b / WAVEFORM_BINS, to = max(from + 1, used * (b + 1) / WAVEFORM_BINS);
        int binPeak = 0;
        for (size_t i = from; i < to && i < used; i++) binPeak = max(binPeak, (int)envelope[i]);
        moment.waveform[b] = (uint8_t)(255 * sqrtf((float)binPeak / peak));
    }
}

static void finishRecording() {
    size_t length = scratchLength;
    if (streamingRecord) {
        state = AUDIO_SAVING;
        uint32_t start = millis();
        Moment moment = streamMoment;
        analyseEnvelope(moment, length);
        bool ok = storageStreamEnd(moment); // sets length and dataOffset
        free(envelope);
        envelope = nullptr;
        streamingRecord = false;
        if (!ok || moment.length < MIN_MOMENT_SAMPLES) {
            if (moment.fileName[0]) storageDeleteMoment(moment);
            state = AUDIO_IDLE;
            Serial.println(ok ? "Recording too short, discarded" : "Recording lost: the SD card didn't take it");
            return;
        }
        nextMomentNumber = moment.number + 1;
        xSemaphoreTake(momentLock, portMAX_DELAY);
        memmove(&moments[1], &moments[0], momentTotal * sizeof(Moment));
        moments[0] = moment;
        momentTotal++;
        listVersion++;
        xSemaphoreGive(momentLock);
        state = AUDIO_IDLE;
        Serial.printf("Saved Moment %d to SD card as %s/%s in %lu ms: %.2f s, playback gain %.1fx, mic gain %d dB%s\n",
                      moment.number, moment.folder, moment.fileName, millis() - start, (float)moment.length / SAMPLE_RATE,
                      moment.gain, gainNow, gainSetting == GAIN_AUTO ? (agcChanges ? " (auto, adjusted)" : " (auto)") : "");
        return;
    }
    if (length < MIN_MOMENT_SAMPLES) {
        free(scratch);
        scratch = nullptr;
        state = AUDIO_IDLE;
        Serial.println("Recording too short, discarded");
        return;
    }
    Moment moment = {};
    moment.number = nextMomentNumber++;
    moment.recordedAt = recordStartedAt;
    moment.startMs = recordStartedMs;
    moment.length = length;
    analyse(moment, scratch);

    // On the card if possible, else (no card, or the write failed) in PSRAM
    state = AUDIO_SAVING;
    uint32_t start = millis();
    bool onCard = storageAvailable() && storageSaveMoment(moment, scratch); // sets moment.fileName
    if (onCard) {
        free(scratch);
    } else {
        // Shrinking in place, so no second copy is needed
        moment.samples = (int16_t *)heap_caps_realloc(scratch, length * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!moment.samples) moment.samples = scratch;
    }
    scratch = nullptr;

    xSemaphoreTake(momentLock, portMAX_DELAY);
    memmove(&moments[1], &moments[0], momentTotal * sizeof(Moment));
    moments[0] = moment;
    momentTotal++;
    listVersion++;
    xSemaphoreGive(momentLock);
    state = AUDIO_IDLE;
    Serial.printf("Saved Moment %d %s%s%s%s in %lu ms: %.2f s, playback gain %.1fx, mic gain %d dB%s\n",
                  moment.number, onCard ? "to SD card as " : "in memory", onCard ? moment.folder : "", onCard ? "/" : "",
                  moment.fileName, millis() - start,
                  (float)length / SAMPLE_RATE, moment.gain, gainNow,
                  gainSetting == GAIN_AUTO ? (agcChanges ? " (auto, adjusted)" : " (auto)") : "");
}

static void startPlayback(int index) {
    if (index < 0 || index >= momentTotal) return;
    if (!moments[index].samples) {
        playFile = storageOpenMoment(moments[index]);
        if (!playFile) {
            Serial.printf("Can't open Moment %d on the SD card\n", moments[index].number);
            return;
        }
        playBufferStart = playBufferLength = 0;
        playFileEnded = false;
    }
    playingIndex = index;
    playPos = 0;
    speed = 1;
    speedDirection = 0;
    digitalWrite(PA_ENABLE_PIN, HIGH);
    state = AUDIO_PLAYING;
    Serial.printf("Playing Moment %d\n", moments[index].number);
}

static void stopPlayback() {
    digitalWrite(PA_ENABLE_PIN, LOW);
    if (playFile) playFile.close();
    playingIndex = -1;
    state = AUDIO_IDLE;
}

static void deleteMoment(int index) {
    if (index < 0 || index >= momentTotal) return;
    if (playingIndex == index) stopPlayback();
    int number = moments[index].number;
    if (moments[index].samples) free(moments[index].samples);
    else if (!storageDeleteMoment(moments[index])) Serial.printf("Couldn't delete Moment %d from the SD card\n", number);

    xSemaphoreTake(momentLock, portMAX_DELAY);
    memmove(&moments[index], &moments[index + 1], (momentTotal - index - 1) * sizeof(Moment));
    momentTotal--;
    if (playingIndex > index) playingIndex--;
    listVersion++;
    xSemaphoreGive(momentLock);
    Serial.printf("Deleted Moment %d\n", number);
}

static void tagMoment(int index, int tag) {
    if (index < 0 || index >= momentTotal || tag < 0 || tag >= TAG_COUNT) return;
    xSemaphoreTake(momentLock, portMAX_DELAY);
    moments[index].tag = tag;
    listVersion++;
    xSemaphoreGive(momentLock);
    if (!moments[index].samples) {
        Moment updated = moments[index];
        if (!storageUpdateMoment(updated)) {
            Serial.printf("Couldn't save the tag of Moment %d to the SD card\n", moments[index].number);
        } else if (strcmp(updated.fileName, moments[index].fileName) != 0) {
            xSemaphoreTake(momentLock, portMAX_DELAY);
            strlcpy(moments[index].fileName, updated.fileName, sizeof(moments[index].fileName));
            listVersion++;
            xSemaphoreGive(momentLock);
        }
    }
}

// The card's moments go with it; those held in memory stay
static void formatCard() {
    if (state == AUDIO_PLAYING) stopPlayback();
    state = AUDIO_FORMATTING;
    bool ok = storageFormat();
    xSemaphoreTake(momentLock, portMAX_DELAY);
    int kept = 0;
    for (int i = 0; i < momentTotal; i++) {
        if (moments[i].samples) moments[kept++] = moments[i];
    }
    momentTotal = kept;
    listVersion++;
    xSemaphoreGive(momentLock);
    formatResult = ok ? 1 : -1;
    state = AUDIO_IDLE;
}

// Self-test: 0.5 s of silence, then 1 s of a 1 kHz tone through the speaker,
// measuring the mic input during each phase. A working speaker + mic shows a
// clearly higher level during the tone.
static const size_t TEST_QUIET_FRAMES = SAMPLE_RATE / 2;
static const size_t TEST_TONE_FRAMES = SAMPLE_RATE;
static size_t testFrame = 0;
static int testPeak = 0;
static double testSumSq = 0;

static void printTestLevel(const char *label, size_t frames) {
    Serial.printf("  %s: peak %d, rms %.0f\n", label, testPeak, sqrt(testSumSq / frames));
    testPeak = 0;
    testSumSq = 0;
}

static void startSelfTest() {
    testFrame = 0;
    testPeak = 0;
    testSumSq = 0;
    digitalWrite(PA_ENABLE_PIN, HIGH);
    state = AUDIO_SELF_TEST;
    Serial.println("Self-test: measuring silence, then a 1 kHz tone...");
}

static void selfTestChunk(const int16_t *in, int16_t *out, size_t frames) {
    for (size_t i = 0; i < frames; i++) {
        size_t frame = testFrame + i;
        if (frame == TEST_QUIET_FRAMES) printTestLevel("silence", TEST_QUIET_FRAMES);
        int s = in[2 * i];
        testPeak = max(testPeak, abs(s));
        testSumSq += (double)s * s;
        int16_t tone = frame < TEST_QUIET_FRAMES ? 0 : (int16_t)(8000 * sin(2 * M_PI * 1000 * frame / SAMPLE_RATE));
        out[2 * i] = out[2 * i + 1] = tone;
    }
    size_t written;
    i2s_write(I2S_NUM_0, out, frames * 4, &written, portMAX_DELAY);
    testFrame += frames;
    if (testFrame >= TEST_QUIET_FRAMES + TEST_TONE_FRAMES) {
        printTestLevel("tone   ", TEST_TONE_FRAMES);
        digitalWrite(PA_ENABLE_PIN, LOW);
        state = AUDIO_IDLE;
    }
}

static void handleCommand(const Command &cmd) {
    switch (cmd.type) {
    case CMD_RECORD_START:
        if (state == AUDIO_PLAYING) stopPlayback();
        if (state == AUDIO_IDLE) {
            audioWake();
            startRecording(cmd.arg > 0 ? cmd.arg : MAX_RECORDING_SECONDS);
        }
        break;
    case CMD_RECORD_STOP:
        if (state == AUDIO_RECORDING) finishRecording();
        break;
    case CMD_PLAY:
        if (state == AUDIO_PLAYING) stopPlayback();
        if (state == AUDIO_IDLE) {
            audioWake();
            startPlayback(cmd.arg);
        }
        break;
    case CMD_STOP_PLAYBACK:
        if (state == AUDIO_PLAYING) stopPlayback();
        break;
    case CMD_DELETE:
        deleteMoment(cmd.arg);
        break;
    case CMD_SELF_TEST:
        if (state == AUDIO_IDLE) {
            audioWake();
            startSelfTest();
        }
        break;
    case CMD_SUSPEND:
        if (state == AUDIO_RECORDING) finishRecording();
        if (state == AUDIO_PLAYING) stopPlayback();
        state = AUDIO_IDLE;
        audioStandby();
        xSemaphoreGive(suspendDone);
        break;
    case CMD_TAG:
        tagMoment(cmd.arg, cmd.arg2);
        break;
    case CMD_FORMAT:
        if (state == AUDIO_IDLE || state == AUDIO_PLAYING) formatCard();
        break;
    case CMD_GAIN:
        gainSetting = cmd.arg;
        if (cmd.arg != GAIN_AUTO) gainNow = cmd.arg;
        if (running) applyGain(gainNow); // else when the codec next powers up
        break;
    }
}

// One sample of the playing moment; from the card through playBuffer
static int16_t sampleAt(const Moment &moment, size_t i) {
    if (moment.samples) return moment.samples[i];
    if (i < playBufferStart || i >= playBufferStart + playBufferLength) {
        // Load the window around i, so playing backwards stays inside it too
        size_t start = i > PLAY_BUFFER_SAMPLES / 2 ? i - PLAY_BUFFER_SAMPLES / 2 : 0;
        size_t want = min(PLAY_BUFFER_SAMPLES, moment.length - start);
        playFile.seek(moment.dataOffset + start * sizeof(int16_t));
        playBufferStart = start;
        playBufferLength = playFile.read((uint8_t *)playBuffer, want * sizeof(int16_t)) / sizeof(int16_t);
        if (playBufferLength == 0) {
            playFileEnded = true; // the file is shorter than its header says
            return 0;
        }
    }
    return playBuffer[i - playBufferStart];
}

static void updateSpeed() {
    int direction = speedDirection;
    float s = speed;
    if (direction != 0) {
        // Towards the held direction: by steps away from zero, straight through
        // zero inside the scrub band, like a reel slowed to a stop and turned
        float towards = direction > 0 ? 1 : -1;
        if (s * towards >= SCRUB_SPEED) s *= SPEED_STEP;          // same way: faster
        else if (s * towards <= -SCRUB_SPEED) s /= SPEED_STEP;    // other way: slower
        else s += towards * SCRUB_STEP;                           // through zero
        s = constrain(s, -MAX_SPEED, MAX_SPEED);
    } else if (fabsf(s - 1) > 0.01f) {
        s = 1 + (s - 1) * SPEED_RETURN; // let go: back to normal, forward
    } else {
        s = 1;
    }
    speed = s;
}

// Fills `out` (stereo) with up to `frames` frames of the playing moment at the
// current speed, by linear interpolation; fewer at the end
static size_t playbackChunk(const Moment &moment, int16_t *out, size_t frames) {
    updateSpeed();
    double pos = playPos;
    float step = speed;
    size_t n = 0;
    while (n < frames && !playFileEnded) {
        if (pos < 0) pos = 0; // backwards to the start: wait there
        size_t i = (size_t)pos;
        if (i + 1 >= moment.length) {
            if (step < 0) i = moment.length - 2; // at the end but running back: carry on from there
            else break;
        }
        float frac = (float)(pos - i);
        float s = (sampleAt(moment, i) * (1 - frac) + sampleAt(moment, i + 1) * frac) * moment.gain;
        out[2 * n] = out[2 * n + 1] = (int16_t)constrain(s, -32768.0f, 32767.0f);
        pos += step;
        n++;
    }
    playPos = pos;
    return n;
}

static void audioTask(void *) {
    static int16_t in[CHUNK_FRAMES * 2];
    static int16_t out[CHUNK_FRAMES * 2];

    for (;;) {
        Command cmd;
        if (!running) {
            if (xQueueReceive(commandQueue, &cmd, portMAX_DELAY) == pdTRUE) handleCommand(cmd);
            idleSince = millis();
            continue;
        }

        // Reading paces the loop: blocks until the next 16 ms of mic audio arrives
        size_t bytesRead = 0;
        i2s_read(I2S_NUM_0, in, sizeof(in), &bytesRead, portMAX_DELAY);
        size_t frames = bytesRead / 4;

        while (xQueueReceive(commandQueue, &cmd, 0) == pdTRUE) handleCommand(cmd);

        size_t skip = min(frames, settleLeft);
        settleLeft -= skip;
        if (state == AUDIO_RECORDING) {
            size_t n = min(frames - skip, recordLimit - scratchLength);
            if (scratchLength == 0 && n > 0 && clockValid()) {
                // The chunk just read ends about now; its first kept sample came earlier
                timeval tv;
                gettimeofday(&tv, nullptr);
                int64_t us = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec - (int64_t)(frames - skip) * 1000000 / SAMPLE_RATE;
                recordStartedAt = us / 1000000;
                recordStartedMs = us / 1000 % 1000;
            }
            if (streamingRecord) {
                if (scratchLength == 0 && n > 0) {
                    streamMoment = {};
                    streamMoment.number = nextMomentNumber;
                    streamMoment.recordedAt = recordStartedAt;
                    streamMoment.startMs = recordStartedMs;
                    if (!storageStreamBegin(streamMoment)) {
                        Serial.println("Can't record onto the SD card");
                        streamMoment.fileName[0] = 0;
                        finishRecording(); // discards it
                        continue;
                    }
                }
                static int16_t mono[CHUNK_FRAMES];
                int chunkPeak = 0;
                for (size_t i = 0; i < n; i++) {
                    mono[i] = in[2 * (skip + i)]; // ES8311 ADC is on the left slot
                    chunkPeak = max(chunkPeak, abs((int)mono[i]));
                }
                storageStreamWrite(mono, n);
                size_t entry = scratchLength / CHUNK_FRAMES / envelopeChunks;
                envelope[entry] = max(envelope[entry], (uint16_t)chunkPeak);
            } else {
                for (size_t i = 0; i < n; i++) {
                    scratch[scratchLength + i] = in[2 * (skip + i)]; // ES8311 ADC is on the left slot
                }
            }
            scratchLength += n;
            if (gainSetting == GAIN_AUTO) autoGain(in, skip + n, skip);
            if (scratchLength >= recordLimit) finishRecording();
        } else if (state == AUDIO_PLAYING) {
            const Moment &moment = moments[playingIndex];
            size_t n = playbackChunk(moment, out, CHUNK_FRAMES);
            size_t written;
            i2s_write(I2S_NUM_0, out, n * 4, &written, portMAX_DELAY);
            if (n < CHUNK_FRAMES) {
                delay(150); // let the DMA buffers drain before muting the amp
                stopPlayback();
            }
        } else if (state == AUDIO_SELF_TEST) {
            selfTestChunk(in, out, frames);
        }

        if (state != AUDIO_IDLE) idleSince = millis();
        else if (millis() - idleSince > STANDBY_AFTER_MS && uxQueueMessagesWaiting(commandQueue) == 0) audioStandby();
    }
}

static bool codecInit() {
    if (!codec) codec = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
    if (!codec) return false;
    const es8311_clock_config_t clk = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = SAMPLE_RATE * MCLK_MULTIPLE,
        .sample_frequency = SAMPLE_RATE,
    };
    if (es8311_init(codec, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) return false;
    es8311_voice_volume_set(codec, SPEAKER_VOLUME, NULL);
    es8311_microphone_config(codec, false); // analog mic
    es8311_adc_volume_set(codec, 0xBF);     // 0 dB, so the gain setting is the whole gain
    applyGain(gainSetting == GAIN_AUTO ? gainNow : gainSetting);
    return true;
}

static bool i2sInit() {
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);
    cfg.sample_rate = SAMPLE_RATE;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.dma_buf_count = 8;
    cfg.dma_buf_len = CHUNK_FRAMES;
    cfg.tx_desc_auto_clear = true; // output silence when nothing is being played
    cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    i2s_pin_config_t pins = {};
    pins.mck_io_num = I2S_MCK_PIN;
    pins.bck_io_num = I2S_BCK_PIN;
    pins.ws_io_num = I2S_WS_PIN;
    pins.data_out_num = I2S_DOUT_PIN;
    pins.data_in_num = I2S_DIN_PIN;

    if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) return false;
    return i2s_set_pin(I2S_NUM_0, &pins) == ESP_OK;
}

bool audioInit() {
    // Created first so the moment getters are safe even if the rest fails
    momentLock = xSemaphoreCreateMutex();
    commandQueue = xQueueCreate(8, sizeof(Command));
    suspendDone = xSemaphoreCreateBinary();
    moments = (Moment *)ps_calloc(MAX_MOMENTS, sizeof(Moment));
    if (!moments) {
        Serial.println("Failed to allocate the moment list");
        return false;
    }

    Preferences prefs;
    prefs.begin("zvukopis", true);
    gainSetting = prefs.getInt("gain", GAIN_AUTO);
    prefs.end();

    pinMode(PA_ENABLE_PIN, OUTPUT);
    digitalWrite(PA_ENABLE_PIN, LOW);

    if (storageInit()) {
        momentTotal = storageLoadMoments(moments, MAX_MOMENTS);
        qsort(moments, momentTotal, sizeof(Moment), [](const void *a, const void *b) {
            return ((const Moment *)b)->number - ((const Moment *)a)->number; // newest first
        });
        if (momentTotal > 0) nextMomentNumber = moments[0].number + 1;
    }

    // Start I2S first so the codec sees MCLK while it is configured
    if (!i2sInit()) {
        Serial.println("I2S init failed");
        return false;
    }
    if (!codecInit()) {
        Serial.println("ES8311 codec init failed");
        return false;
    }
    running = true;
    idleSince = millis();
    // 8 KB: saving builds the WAV header and card paths on this stack, and the
    // card's long-file-name code adds its own buffers
    xTaskCreatePinnedToCore(audioTask, "audio", 8192, NULL, 5, NULL, 0);
    return true;
}

static void sendCommand(CommandType type, int arg = 0, int arg2 = 0) {
    Command cmd = {type, arg, arg2};
    xQueueSend(commandQueue, &cmd, 0);
}

void audioStartRecording(uint32_t seconds) { sendCommand(CMD_RECORD_START, seconds); }
void audioStopRecording() { sendCommand(CMD_RECORD_STOP); }
void audioPlay(int index) { sendCommand(CMD_PLAY, index); }
void audioStopPlayback() { sendCommand(CMD_STOP_PLAYBACK); }
void audioDelete(int index) { sendCommand(CMD_DELETE, index); }
void audioSelfTest() { sendCommand(CMD_SELF_TEST); }
void audioSetTag(int index, MomentTag tag) { sendCommand(CMD_TAG, index, tag); }
void audioSpeedHold(int direction) { speedDirection = direction; }

void audioSetMicGain(int db) {
    gainSetting = db;
    Preferences prefs;
    prefs.begin("zvukopis", false);
    prefs.putInt("gain", db);
    prefs.end();
    sendCommand(CMD_GAIN, db);
}

int audioMicGainSetting() { return gainSetting; }
int audioMicGain() { return gainNow; }

void audioFormatCard() {
    formatResult = 0;
    sendCommand(CMD_FORMAT);
}

int audioFormatResult() { return formatResult; }

void audioSuspend() {
    xSemaphoreTake(suspendDone, 0);
    sendCommand(CMD_SUSPEND);
    // Waits for the audio task, which may be saving a long recording first
    if (xSemaphoreTake(suspendDone, pdMS_TO_TICKS(15000)) != pdTRUE) Serial.println("Audio suspend timed out");
}

bool audioAsleep() { return !running && uxQueueMessagesWaiting(commandQueue) == 0; }

AudioState audioState() { return state; }

float audioRecordingSeconds() {
    return state == AUDIO_RECORDING || state == AUDIO_SAVING ? (float)scratchLength / SAMPLE_RATE : 0;
}

float audioRecordingLimitSeconds() { return (float)recordLimit / SAMPLE_RATE; }

int audioPlayingIndex() { return playingIndex; }

float audioPlaybackSeconds() {
    return state == AUDIO_PLAYING ? (float)(playPos / SAMPLE_RATE) : 0;
}

float audioPlaybackSpeed() { return state == AUDIO_PLAYING ? speed : 1; }

float audioSecondsLeft() { return (float)samplesLeft() / SAMPLE_RATE; }

float audioLongestRecording() { return (float)recordableSamples() / SAMPLE_RATE; }

bool audioUsingSdCard() { return storageAvailable(); }

int momentCount() { return momentTotal; }

bool momentGet(int index, Moment &out) {
    bool found = false;
    xSemaphoreTake(momentLock, portMAX_DELAY);
    if (index >= 0 && index < momentTotal) {
        out = moments[index];
        found = true;
    }
    xSemaphoreGive(momentLock);
    return found;
}

uint32_t momentListVersion() { return listVersion; }
