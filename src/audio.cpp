#include "audio.h"

#include <Arduino.h>
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
static const es8311_mic_gain_t MIC_GAIN = ES8311_MIC_GAIN_24DB;
// Playback boosts each memo so its loudest moment is near full scale, up to this much
static const float MAX_PLAYBACK_GAIN = 8.0f;

static const size_t MAX_MEMO_SAMPLES = (size_t)SAMPLE_RATE * MAX_MEMO_SECONDS;
static const size_t MIN_MEMO_SAMPLES = SAMPLE_RATE / 4; // anything shorter is an accidental tap
static const size_t PSRAM_RESERVE = 256 * 1024;         // left free for the rest of the app
static const uint64_t SD_RESERVE = 1024 * 1024;         // for file system overhead
static const int CHUNK_FRAMES = SAMPLE_RATE / 1000 * 16; // 16 ms
static const size_t PLAY_BUFFER_SAMPLES = 16 * CHUNK_FRAMES; // read from the SD card 256 ms at a time
static const uint32_t STANDBY_AFTER_MS = 2000;               // idle time before the codec powers down

enum CommandType : uint8_t {
    CMD_RECORD_START,
    CMD_RECORD_STOP,
    CMD_PLAY,
    CMD_STOP_PLAYBACK,
    CMD_DELETE,
    CMD_SELF_TEST,
    CMD_SUSPEND,
};

struct Command {
    CommandType type;
    int arg;
};

static QueueHandle_t commandQueue;

// The memo list is only modified by the audio task; the lock keeps readers on
// other tasks from seeing it half-updated
static SemaphoreHandle_t memoLock;
static Memo *memos = nullptr; // MAX_MEMOS entries, in PSRAM
static volatile int memoTotal = 0;
static volatile uint32_t listVersion = 0;
static int nextMemoNumber = 1;

// Recording goes into a buffer sized for the longest memo that fits, which is
// then shrunk to the memo (in PSRAM) or written to the card and freed.
// At 48 kHz a minute is 5.8 MB, too much to keep reserved.
static int16_t *scratch = nullptr;
static volatile size_t scratchLength = 0;
static volatile size_t recordLimit = 0;
static time_t recordStartedAt = 0;

static volatile AudioState state = AUDIO_IDLE;
static volatile int playingIndex = -1;
static volatile size_t playPos = 0;

// Playback of memos on the SD card streams through this buffer
static File playFile;
static int16_t playBuffer[PLAY_BUFFER_SAMPLES];
static size_t playBufferStart = 0, playBufferLength = 0; // in samples of the memo

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
    if (memoTotal >= MAX_MEMOS) return 0;
    size_t ramBytes = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    uint64_t ramSamples = ramBytes > PSRAM_RESERVE ? (ramBytes - PSRAM_RESERVE) / sizeof(int16_t) : 0;
    if (!storageAvailable()) return ramSamples;
    uint64_t freeBytes = storageFreeBytes();
    uint64_t cardSamples = freeBytes > SD_RESERVE ? (freeBytes - SD_RESERVE) / sizeof(int16_t) : 0;
    // Saved memos only take room on the card; PSRAM just has to hold one recording
    return ramSamples >= MAX_MEMO_SAMPLES ? cardSamples : min(cardSamples, ramSamples);
}

static void startRecording() {
    recordLimit = (size_t)min((uint64_t)MAX_MEMO_SAMPLES, samplesLeft());
    if (recordLimit < MIN_MEMO_SAMPLES) {
        Serial.println("No room for another recording");
        return;
    }
    scratch = (int16_t *)ps_malloc(recordLimit * sizeof(int16_t));
    if (!scratch) {
        Serial.println("Can't allocate the recording buffer");
        return;
    }
    scratchLength = 0;
    recordStartedAt = clockValid() ? time(nullptr) : 0;
    state = AUDIO_RECORDING;
    Serial.printf("Recording started at %lu ms\n", millis());
}

// Playback gain and list waveform
static void analyse(Memo &memo, const int16_t *samples) {
    size_t length = memo.length;
    int peak = 1;
    for (size_t i = 0; i < length; i++) peak = max(peak, abs((int)samples[i]));
    memo.gain = min(MAX_PLAYBACK_GAIN, 29000.0f / peak);
    for (int b = 0; b < WAVEFORM_BINS; b++) {
        size_t from = length * b / WAVEFORM_BINS, to = length * (b + 1) / WAVEFORM_BINS;
        int binPeak = 0;
        for (size_t i = from; i < to; i++) binPeak = max(binPeak, abs((int)samples[i]));
        // sqrt keeps quiet passages visible next to loud ones
        memo.waveform[b] = (uint8_t)(255 * sqrtf((float)binPeak / peak));
    }
}

static void finishRecording() {
    size_t length = scratchLength;
    if (length < MIN_MEMO_SAMPLES) {
        free(scratch);
        scratch = nullptr;
        state = AUDIO_IDLE;
        Serial.println("Recording too short, discarded");
        return;
    }
    Memo memo = {};
    memo.number = nextMemoNumber++;
    memo.recordedAt = recordStartedAt;
    memo.length = length;
    analyse(memo, scratch);

    // On the card if possible, else (no card, or the write failed) in PSRAM
    state = AUDIO_SAVING;
    uint32_t start = millis();
    bool onCard = storageAvailable() && storageSaveMemo(memo, scratch); // sets memo.fileName
    if (onCard) {
        free(scratch);
    } else {
        // Shrinking in place, so no second copy is needed
        memo.samples = (int16_t *)heap_caps_realloc(scratch, length * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!memo.samples) memo.samples = scratch;
    }
    scratch = nullptr;

    xSemaphoreTake(memoLock, portMAX_DELAY);
    memmove(&memos[1], &memos[0], memoTotal * sizeof(Memo));
    memos[0] = memo;
    memoTotal++;
    listVersion++;
    xSemaphoreGive(memoLock);
    state = AUDIO_IDLE;
    Serial.printf("Saved Memo %d %s%s in %lu ms: %.2f s, playback gain %.1fx\n",
                  memo.number, onCard ? "to SD card as " : "in memory", memo.fileName, millis() - start,
                  (float)length / SAMPLE_RATE, memo.gain);
}

static void startPlayback(int index) {
    if (index < 0 || index >= memoTotal) return;
    if (!memos[index].samples) {
        playFile = storageOpenMemo(memos[index]);
        if (!playFile) {
            Serial.printf("Can't open Memo %d on the SD card\n", memos[index].number);
            return;
        }
        playBufferStart = playBufferLength = 0;
    }
    playingIndex = index;
    playPos = 0;
    digitalWrite(PA_ENABLE_PIN, HIGH);
    state = AUDIO_PLAYING;
    Serial.printf("Playing Memo %d\n", memos[index].number);
}

static void stopPlayback() {
    digitalWrite(PA_ENABLE_PIN, LOW);
    if (playFile) playFile.close();
    playingIndex = -1;
    state = AUDIO_IDLE;
}

static void deleteMemo(int index) {
    if (index < 0 || index >= memoTotal) return;
    if (playingIndex == index) stopPlayback();
    int number = memos[index].number;
    if (memos[index].samples) free(memos[index].samples);
    else if (!storageDeleteMemo(memos[index])) Serial.printf("Couldn't delete Memo %d from the SD card\n", number);

    xSemaphoreTake(memoLock, portMAX_DELAY);
    memmove(&memos[index], &memos[index + 1], (memoTotal - index - 1) * sizeof(Memo));
    memoTotal--;
    if (playingIndex > index) playingIndex--;
    listVersion++;
    xSemaphoreGive(memoLock);
    Serial.printf("Deleted Memo %d\n", number);
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
            startRecording();
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
        deleteMemo(cmd.arg);
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
    }
}

// Next stretch of the playing memo, at most `max` samples; 0 at the end
static size_t nextPlaybackSamples(const Memo &memo, size_t max, const int16_t *&src) {
    size_t left = memo.length - playPos;
    if (memo.samples) {
        src = memo.samples + playPos;
        return min(max, left);
    }
    if (playPos >= playBufferStart + playBufferLength) {
        size_t want = min(PLAY_BUFFER_SAMPLES, left);
        playBufferStart = playPos;
        playBufferLength = playFile.read((uint8_t *)playBuffer, want * sizeof(int16_t)) / sizeof(int16_t);
    }
    src = playBuffer + (playPos - playBufferStart);
    return min(max, playBufferStart + playBufferLength - playPos);
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
            for (size_t i = 0; i < n; i++) {
                scratch[scratchLength + i] = in[2 * (skip + i)]; // ES8311 ADC is on the left slot
            }
            scratchLength += n;
            if (scratchLength >= recordLimit) finishRecording();
        } else if (state == AUDIO_PLAYING) {
            const Memo &memo = memos[playingIndex];
            const int16_t *src;
            size_t n = nextPlaybackSamples(memo, CHUNK_FRAMES, src);
            for (size_t i = 0; i < n; i++) {
                float s = src[i] * memo.gain;
                out[2 * i] = out[2 * i + 1] = (int16_t)constrain(s, -32768.0f, 32767.0f);
            }
            size_t written;
            i2s_write(I2S_NUM_0, out, n * 4, &written, portMAX_DELAY);
            playPos += n;
            if (n == 0 || playPos >= memo.length) { // n == 0: the file ended early
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
    es8311_microphone_gain_set(codec, MIC_GAIN);
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
    // Created first so the memo getters are safe even if the rest fails
    memoLock = xSemaphoreCreateMutex();
    commandQueue = xQueueCreate(8, sizeof(Command));
    suspendDone = xSemaphoreCreateBinary();
    memos = (Memo *)ps_calloc(MAX_MEMOS, sizeof(Memo));
    if (!memos) {
        Serial.println("Failed to allocate the memo list");
        return false;
    }

    pinMode(PA_ENABLE_PIN, OUTPUT);
    digitalWrite(PA_ENABLE_PIN, LOW);

    if (storageInit()) {
        memoTotal = storageLoadMemos(memos, MAX_MEMOS);
        qsort(memos, memoTotal, sizeof(Memo), [](const void *a, const void *b) {
            return ((const Memo *)b)->number - ((const Memo *)a)->number; // newest first
        });
        if (memoTotal > 0) nextMemoNumber = memos[0].number + 1;
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
    xTaskCreatePinnedToCore(audioTask, "audio", 4096, NULL, 5, NULL, 0);
    return true;
}

static void sendCommand(CommandType type, int arg = 0) {
    Command cmd = {type, arg};
    xQueueSend(commandQueue, &cmd, 0);
}

void audioStartRecording() { sendCommand(CMD_RECORD_START); }
void audioStopRecording() { sendCommand(CMD_RECORD_STOP); }
void audioPlay(int index) { sendCommand(CMD_PLAY, index); }
void audioStopPlayback() { sendCommand(CMD_STOP_PLAYBACK); }
void audioDelete(int index) { sendCommand(CMD_DELETE, index); }
void audioSelfTest() { sendCommand(CMD_SELF_TEST); }

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
    return state == AUDIO_PLAYING ? (float)playPos / SAMPLE_RATE : 0;
}

float audioSecondsLeft() { return (float)samplesLeft() / SAMPLE_RATE; }

bool audioUsingSdCard() { return storageAvailable(); }

int memoCount() { return memoTotal; }

bool memoGet(int index, Memo &out) {
    bool found = false;
    xSemaphoreTake(memoLock, portMAX_DELAY);
    if (index >= 0 && index < memoTotal) {
        out = memos[index];
        found = true;
    }
    xSemaphoreGive(memoLock);
    return found;
}

uint32_t memoListVersion() { return listVersion; }
