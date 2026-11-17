#include "audio.h"

#include <Arduino.h>
#include <Preferences.h>
#include <driver/i2s.h>
#include <math.h>

#include "clock.h"
#include "es8311.h"
#include "storage.h"
#include "usbmic.h"

static const int I2S_MCK_PIN = 13;
static const int I2S_BCK_PIN = 14;
static const int I2S_WS_PIN = 47;
static const int I2S_DOUT_PIN = 48; // to codec DAC (speaker)
static const int I2S_DIN_PIN = 21;  // from codec ADC (mic)
static const int PA_ENABLE_PIN = 39; // speaker amplifier

// The codec's clock table has 96 kHz only with a 12.288 MHz master clock
static const int MCLK_MULTIPLE = SAMPLE_RATE >= 96000 ? 128 : 256;
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
// Playback from the card goes through a ring in PSRAM that a reader task
// (core 1) keeps filled around the playhead, half a second per card read, so
// the audio task never waits on the card
static const size_t RING_SAMPLES = SAMPLE_RATE * 4;   // 4 s
static const size_t READ_BLOCK = SAMPLE_RATE / 2;     // per card read
static const size_t AHEAD_SAMPLES = SAMPLE_RATE * 2;  // kept loaded in the direction of travel
static const uint32_t STANDBY_AFTER_MS = 2000;        // idle time before the codec powers down
// Varispeed, per 16 ms chunk: holding a key multiplies or divides the speed by
// SPEED_STEP (so it keeps accelerating, like a tape reel), and on release it
// eases back to normal
// Settings (Settings > Varispeed, or the web interface): the top speed, the
// ramp, which picks a column below, and what letting go does: back to normal,
// or the speed stays (a tap brings it back then)
static volatile int maxSpeedSetting = 3;
static volatile int rampSetting = RAMP_NORMAL;
static volatile bool holdSetting = false;
static volatile bool returning = false;     // in hold mode, after a tap: gliding back to 1x
static volatile float speedAtPress = 1;     // where the speed was when the rocker went down
static volatile bool restoreSpeed = false;  // a tap: back to speedAtPress first, then the glide
static const float SPEED_STEP[RAMP_COUNT] = {1.02f, 1.04f, 1.08f};   // normal: 2x after ~0.3 s held, 3x after ~0.45 s
static const float SPEED_RETURN[RAMP_COUNT] = {0.92f, 0.85f, 0.75f}; // fraction of the distance to 1x left after each chunk
// Below this the speed stops shrinking by steps and slides straight through
// zero: the tape stops, then runs backwards (and on up to the top speed while held)
static const float SCRUB_SPEED = 0.25f;
static const float SCRUB_STEP[RAMP_COUNT] = {0.03f, 0.05f, 0.09f}; // per chunk: normal is 0.16 s from +0.25 to -0.25
// With the speed staying, a moment left running backwards waits this long at
// its start and then ends (playbackChunk); the count is the audio task's alone
static const int START_WAIT_CHUNKS = 3000 / 16;
static int waitedAtStart = 0;

// Play, delete and tag name their moment by its number, never by its place in
// the list: the place is found when the request is carried out, since the
// list may have shifted by then (a recording saved, another moment deleted)
enum CommandType : uint8_t {
    CMD_RECORD_START,
    CMD_RECORD_STOP,
    CMD_RECORD_CANCEL,
    CMD_PLAY,
    CMD_STOP_PLAYBACK,
    CMD_DELETE,
    CMD_SELF_TEST,
    CMD_SUSPEND,
    CMD_TAG,
    CMD_TAGS_ABOVE, // untag every moment whose slot is above arg
    CMD_FORMAT,
    CMD_GAIN,
    CMD_USB_MIC,
    CMD_RECHECK_CARD,
};

struct Command {
    CommandType type;
    int arg;
    int arg2;
};

static QueueHandle_t commandQueue;
static volatile bool ready = false; // the audio task runs: without it nobody would take the requests

// The moment list is only modified by the audio task; the lock keeps readers on
// other tasks from seeing it half-updated
static SemaphoreHandle_t momentLock;
static Moment *moments = nullptr; // MAX_MOMENTS entries, in PSRAM
static volatile int momentTotal = 0;
static volatile uint32_t listVersion = 0;
static int nextMomentNumber = 1;

// Recording goes into a buffer sized for the longest moment that fits, which is
// then shrunk to the moment (in PSRAM) or written to the card and freed.
// At 96 kHz a minute is 11.5 MB, too much to keep reserved.
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
static volatile AudioNotice notice = AUDIO_NOTICE_NONE;
static volatile uint32_t noticeSince = 0;
static volatile int playingIndex = -1;
static volatile double playPos = 0; // in samples; fractional with varispeed
static volatile float speed = 1;
static volatile int speedDirection = 0;
static volatile int formatResult = 0;
static volatile int gainSetting = GAIN_AUTO; // dB, or GAIN_AUTO
static volatile int gainNow = DEFAULT_AUTO_GAIN_DB; // what the codec is set to
static int agcQuietChunks = 0;
static int agcChanges = 0;

// Playback of moments on the SD card: the reader task owns the file and
// loads the ring; the window [winStart, winEnd) is what's loaded, in samples
// of the moment. The reader moves one end, then the other, so that the two
// read separately by the audio task always bound a loaded range.
// The audio task opens the file and hands it over in nextFile (under
// momentLock): looking a file up by name is what goes deepest into the card
// code, deeper still when the card doesn't answer and the driver reports it,
// and the audio task's stack has the room for that. The reader then only
// seeks and reads, well within its own.
static int16_t *ring = nullptr;                 // RING_SAMPLES, in PSRAM
static volatile size_t winStart = 0, winEnd = 0;
static volatile size_t fileEnd = 0;             // the moment's length, less if the file is shorter
static volatile int readerMoment = -1;          // the index the reader serves, -1 none
static volatile uint32_t readerSeq = 0;         // bumped on every start or stop
static File nextFile;                           // opened for the reader, not yet taken by it
static File playFile;
static TaskHandle_t readerTaskHandle = nullptr;
static SemaphoreHandle_t firstBlock;            // given by the reader once a new playback has sound
static SemaphoreHandle_t readerIdle;            // given by the reader once it has let go of a file
static volatile uint32_t underruns = 0;
static float stepBefore;                        // playbackChunk's glide

static es8311_handle_t codec = nullptr;

// Auto gain climbs through the codec's fine ADC volume (REG17, 0.5 dB per
// step), which the codec itself fades towards whatever is written, sample
// by sample. The ES8311 header calls the setting 0.25 dB per 4096 samples;
// measured on a tone, it's half that speed: about 3 dB per second at
// 96 kHz. A lull points that volume at the top and the gain glides up
// until a sound holds it. The coarse registers (analog preamp, digital
// gain) only move on an attack, instantly, with the fade switched off for
// that write. The firmware keeps an estimate of where the fade has got to.
static const uint8_t ADC_VOLUME_0DB = 0xBF;
static const es8311_fade_t FINE_FADE = ES8311_FADE_4096LRCK;
static const int FINE_FADE_SAMPLES = 8192;  // per 0.25 dB, as measured (2026-11-02, 1 kHz tone: 18.8 dB in 6.0 s)
static const int FINE_MAX_HALF_DB = 2 * 24; // the fine volume reaches 24 dB above the coarse gain
static volatile int coarseDb = 36;          // what the preamp and digital gain registers are set to
static int fineTargetHalfDb = 0;            // what the fine volume register is set to, in half-decibels above 0 dB
static float fineHalfDb = 0;                // where the codec's fade is now, by the firmware's reckoning

static void setFine(int halfDb, bool instant) {
    fineTargetHalfDb = constrain(halfDb, 0, FINE_MAX_HALF_DB);
    if (instant) fineHalfDb = fineTargetHalfDb;
    if (!codec) return;
    es8311_adc_volume_set(codec, ADC_VOLUME_0DB + fineTargetHalfDb);
    if (instant) { // the new value first, then off with the fade: the volume snaps to it, never to an old target
        es8311_microphone_fade(codec, ES8311_FADE_OFF);
        es8311_microphone_fade(codec, FINE_FADE);
    }
}
static int gainOf(int coarse, float fineHalf) { return coarse + (int)lroundf(fineHalf / 2); }
static SemaphoreHandle_t suspendDone;
// Codec powered and I2S streaming. Off while idle: the codec, mic and I2S DMA
// draw power, and the task would otherwise wake every 16 ms.
static volatile bool running = false;
static uint32_t idleSince = 0;
// After power-up the ADC gives ~70 ms of silence with a click, then settles
static const size_t SETTLE_SAMPLES = SAMPLE_RATE / 10;
static size_t settleLeft = 0; // samples still to drop from the start of a recording

static bool codecInit();
static bool recheckCard();

// Nearest gain the codec can do: 3 dB steps up to 30, then 6 dB steps
static int gainStepUp(int db) { return db < 30 ? db + 3 : db + 6; }
static int gainStepDown(int db) { return db > 30 ? db - 6 : db - 3; }

// The coarse gain: the analog preamp and the digital gain registers; the fine
// volume goes back to 0 dB at the same time
static void applyGain(int db) {
    db = constrain(db, MIN_GAIN_DB, MAX_GAIN_DB);
    int pga = min(db, 30) / 3;
    int scale = (db - pga * 3) / 6;
    setFine(0, true); // first, so that the moment between the writes is a dip, not a burst
    if (codec) {
        es8311_microphone_pga_set(codec, pga);
        es8311_microphone_gain_set(codec, (es8311_mic_gain_t)scale);
    }
    coarseDb = pga * 3 + scale * 6;
    gainNow = coarseDb;
}

// Per 16 ms chunk while recording with auto gain
static void autoGain(const int16_t *in, size_t frames, size_t skip) {
    int peak = 0;
    for (size_t i = skip; i < frames; i++) peak = max(peak, abs((int)in[2 * i]));
    // Where the codec's fade has got to by now
    float perChunk = 0.5f * frames / FINE_FADE_SAMPLES; // half-decibels per chunk
    if (fineHalfDb < fineTargetHalfDb) fineHalfDb = min((float)fineTargetHalfDb, fineHalfDb + perChunk);
    else if (fineHalfDb > fineTargetHalfDb) fineHalfDb = max((float)fineTargetHalfDb, fineHalfDb - perChunk);
    gainNow = gainOf(coarseDb, fineHalfDb);
    if (peak > AGC_LOUD && gainNow > MIN_GAIN_DB) {
        // Attack: at once, and any climb is over. Off the fine volume first, then the coarse registers
        int step = gainNow > 30 ? 6 : 3;
        int fine = (int)fineHalfDb; // rounded down: rather a touch more off than less
        if (fine >= 2 * step) setFine(fine - 2 * step, true);
        else applyGain(coarseDb + fine / 2 - step);
        gainNow = gainOf(coarseDb, fineHalfDb);
        agcQuietChunks = 0;
        agcChanges++;
    } else if (peak < AGC_QUIET) {
        if (++agcQuietChunks >= AGC_RELEASE_CHUNKS && gainNow < MAX_GAIN_DB) {
            // Release: point the fine volume at the top and let the codec glide there
            int top = min(FINE_MAX_HALF_DB, 2 * (MAX_GAIN_DB - coarseDb));
            if (fineTargetHalfDb < top) {
                setFine(top, false);
                Serial.printf("[agc] climbing from %d dB, about %d dB/s\n", gainNow, (SAMPLE_RATE_HZ + 2 * FINE_FADE_SAMPLES) / (4 * FINE_FADE_SAMPLES));
                agcChanges++;
            } else if (fineHalfDb >= top) {
                // The fine volume is used up: the coarse registers take it all over (rare) and the climb goes on
                applyGain(gainNow);
                setFine(min(FINE_MAX_HALF_DB, 2 * (MAX_GAIN_DB - coarseDb)), false);
            }
        }
    } else {
        // A sound, not a loud one: the climb holds where it is
        int here = (int)ceilf(fineHalfDb);
        if (fineTargetHalfDb > here) {
            setFine(here, false);
            Serial.printf("[agc] holding at %d dB\n", gainNow);
        }
        agcQuietChunks = 0;
    }
}

static void setNotice(AudioNotice what) {
    noticeSince = millis();
    notice = what;
}

// False when the codec doesn't answer: what it would deliver then is not a
// recording, so nothing is started
static bool audioWake() {
    if (running) return true;
    uint32_t start = millis();
    i2s_zero_dma_buffer(I2S_NUM_0);
    i2s_start(I2S_NUM_0);
    if (!codecInit() && !codecInit()) { // twice: one answer lost on the I2C bus should not cost a recording
        Serial.println("ES8311 codec init failed");
        i2s_stop(I2S_NUM_0);
        return false;
    }
    running = true;
    settleLeft = SETTLE_SAMPLES;
    Serial.printf("[audio] codec on in %lu ms\n", millis() - start);
    return true;
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

static bool fileRefused = false; // by prepareRecording: there is a card, but it gave no file
static bool freshStart = false;  // a recording was set up since the audio task last looked

// Everything a recording needs, before a sample of it is taken: the room, the
// buffers and, with a card, the file and the task that writes it. Making the
// file takes the card tens of milliseconds, longer for the first of a day;
// done between two chunks of the recording, that much sound went missing.
static bool prepareRecording(uint32_t seconds) {
    fileRefused = false;
    recordLimit = (size_t)min((uint64_t)seconds * SAMPLE_RATE, (uint64_t)recordableSamples());
    if (recordLimit < MIN_MOMENT_SAMPLES) {
        Serial.println("No room for another recording");
        return false;
    }
    streamingRecord = storageAvailable();
    streamMoment = {};
    if (!streamingRecord) {
        scratch = (int16_t *)ps_malloc(recordLimit * sizeof(int16_t));
        if (!scratch) Serial.println("Can't allocate the recording buffer");
        return scratch != nullptr;
    }
    size_t chunks = recordLimit / CHUNK_FRAMES + 1;
    envelopeChunks = (chunks + ENVELOPE_MAX - 1) / ENVELOPE_MAX;
    free(envelope);
    envelope = nullptr;
    // Moments held in memory can have left too little PSRAM for an envelope
    // this fine (a card put in after a start without one): a coarser one
    // gives a flatter list waveform for a short take, which beats no take
    while (!(envelope = (uint16_t *)ps_calloc(chunks / envelopeChunks + 1, sizeof(uint16_t))) && chunks / envelopeChunks > 4 * WAVEFORM_BINS) {
        envelopeChunks *= 2;
    }
    if (!envelope) {
        Serial.println("Can't allocate the recording envelope");
        streamingRecord = false;
        return false;
    }
    // The file takes its name from the time now. The exact start is known
    // with the first sample, and goes into the header when the recording ends.
    streamMoment.number = nextMomentNumber;
    streamMoment.sampleRate = SAMPLE_RATE;
    streamMoment.micGainDb = gainNow;
    streamMoment.micGainAuto = gainSetting == GAIN_AUTO;
    if (clockValid()) {
        timeval tv;
        gettimeofday(&tv, nullptr);
        streamMoment.recordedAt = tv.tv_sec;
        streamMoment.startMs = tv.tv_usec / 1000;
    }
    if (storageStreamBegin(streamMoment)) return true;
    Serial.println("Can't record onto the SD card");
    free(envelope);
    envelope = nullptr;
    streamingRecord = false;
    fileRefused = true;
    return false;
}

// The microphone went on delivering while the card made the file. That sound
// is dropped (it counts as settling time), so that the next chunk read is the
// present and the start time taken from it is right.
static void dropBacklog() {
    int16_t stale[256];
    size_t got, frames = 0;
    do {
        got = 0;
        i2s_read(I2S_NUM_0, stale, sizeof(stale), &got, 0);
        frames += got / 4;
    } while (got == sizeof(stale));
    settleLeft -= min(settleLeft, frames);
}

// False when there is no room, or nothing to record into
static bool startRecording(uint32_t seconds) {
    if (fineTargetHalfDb > (int)ceilf(fineHalfDb)) setFine((int)ceilf(fineHalfDb), false); // a climb in progress holds
    if (!prepareRecording(seconds)) {
        // A card that gives no file may have been taken out and put back,
        // which only asking it shows. Mounted afresh, it takes the recording
        // after all; if it is gone, memory does. Whatever stops the start,
        // the screen gets a line for it (unless the card being gone already
        // has one): the state has said "recording" for a moment by now.
        if (!fileRefused || !recheckCard() || !prepareRecording(seconds)) {
            if (notice == AUDIO_NOTICE_NONE) setNotice(AUDIO_NOTICE_NOT_SAVED);
            return false;
        }
    }
    agcQuietChunks = agcChanges = 0;
    recordStartedAt = 0; // set when the first sample arrives
    recordStartedMs = 0;
    dropBacklog();
    freshStart = true;
    Serial.printf("Recording started at %lu ms\n", millis());
    return true;
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

// Playback gain and list waveform of a moment on the card that has none yet:
// a recording that was cut off, found again when the card was read. A short
// one is read whole. Of a long one, a stretch at the start of each waveform
// column, for as long as FILE_ANALYSIS_MS lasts; the columns that time
// doesn't reach are drawn flat, at the level of the others. The gain is only
// raised when every sample was seen, since a peak that wasn't would clip.
// False when nothing could be read.
static const size_t FILE_ANALYSIS_STRETCH = 8192; // samples
static const uint32_t FILE_ANALYSIS_MS = 1500;    // per file: this runs at start-up, before the first screen

static bool analyseFile(Moment &moment) {
    int16_t *stretch = (int16_t *)ps_malloc(FILE_ANALYSIS_STRETCH * sizeof(int16_t));
    File f = storageOpenMoment(moment);
    uint16_t peaks[WAVEFORM_BINS] = {};
    int peak = 1, bins = 0;
    bool whole = true;
    uint32_t start = millis();
    while (stretch && f && bins < WAVEFORM_BINS && millis() - start < FILE_ANALYSIS_MS) {
        size_t from = (uint64_t)moment.length * bins / WAVEFORM_BINS, to = (uint64_t)moment.length * (bins + 1) / WAVEFORM_BINS;
        size_t n = min(to - from, FILE_ANALYSIS_STRETCH);
        if (!f.seek(moment.dataOffset + from * sizeof(int16_t))) break;
        n = f.read((uint8_t *)stretch, n * sizeof(int16_t)) / sizeof(int16_t);
        if (n < to - from) whole = false;
        int binPeak = 0;
        for (size_t i = 0; i < n; i++) binPeak = max(binPeak, abs((int)stretch[i]));
        peaks[bins++] = binPeak;
        peak = max(peak, binPeak);
    }
    if (f) f.close();
    free(stretch);
    if (bins == 0) return false;
    moment.gain = whole && bins == WAVEFORM_BINS ? min(MAX_PLAYBACK_GAIN, 29000.0f / peak) : 1;
    uint32_t sum = 0;
    for (int b = 0; b < bins; b++) sum += peaks[b];
    for (int b = bins; b < WAVEFORM_BINS; b++) peaks[b] = sum / bins;
    for (int b = 0; b < WAVEFORM_BINS; b++) moment.waveform[b] = (uint8_t)(255 * sqrtf((float)peaks[b] / peak));
    return true;
}

// The card's moments into the list, behind those held in memory: at start-up,
// and again when a card has been mounted afresh or has gone
static void loadCardMoments() {
    xSemaphoreTake(momentLock, portMAX_DELAY);
    int kept = 0;
    for (int i = 0; i < momentTotal; i++) {
        if (moments[i].samples) moments[kept++] = moments[i];
    }
    momentTotal = kept;
    listVersion++;
    xSemaphoreGive(momentLock);

    // Read in behind the end of the list, where no other task looks: with
    // hundreds of files on the card this takes seconds
    Moment *fresh = moments + kept;
    int loaded = storageLoadMoments(fresh, MAX_MOMENTS - kept);
    for (int i = 0; i < loaded; i++) {
        if (fresh[i].gain != 0) continue;
        // A recording that was cut off: what a finished one gets at its end is worked out now, and kept in its file
        if (analyseFile(fresh[i])) storageUpdateMoment(fresh[i]);
        else fresh[i].gain = 1; // plays as it was recorded; the next start tries again
    }

    xSemaphoreTake(momentLock, portMAX_DELAY);
    momentTotal = kept + loaded;
    auto newestFirst = [](const void *a, const void *b) { return ((const Moment *)b)->number - ((const Moment *)a)->number; };
    qsort(moments, momentTotal, sizeof(Moment), newestFirst);
    // Files numbered by a firmware that didn't see the others (a different
    // sample rate, once) can share a number; the later ones get new ones,
    // written into their files so the numbers stay put from then on
    int highest = momentTotal > 0 ? moments[0].number : 0;
    int previousOriginal = highest; // the number the entry before had on the card
    bool renumbered = false;
    for (int i = 1; i < momentTotal; i++) {
        int original = moments[i].number;
        if (original == previousOriginal) {
            Serial.printf("Moment %d twice on the card: %s/%s becomes Moment %d\n", moments[i].number, moments[i].folder, moments[i].fileName, highest + 1);
            moments[i].number = ++highest;
            if (!moments[i].samples && !storageUpdateMoment(moments[i])) Serial.println("  (couldn't write the new number to the card)");
            renumbered = true;
        }
        previousOriginal = original;
    }
    if (renumbered) qsort(moments, momentTotal, sizeof(Moment), newestFirst);
    if (momentTotal > 0) nextMomentNumber = max(nextMomentNumber, moments[0].number + 1);
    listVersion++;
    xSemaphoreGive(momentLock);
}

static void stopPlayback();

// Asks the card whether it is still there, or has come (storageRecheck).
// When that has changed the list is read again, and this is true.
static bool recheckCard() {
    bool had = storageAvailable();
    if (state == AUDIO_PLAYING) stopPlayback(); // the reader lets go of its file before the card is started again
    if (!storageRecheck()) return false;
    loadCardMoments();
    if (had && !storageAvailable()) {
        Serial.println("[audio] the SD card is gone, and its moments are off the list");
        setNotice(AUDIO_NOTICE_NO_CARD);
    } else if (storageAvailable() && notice == AUDIO_NOTICE_NO_CARD) {
        setNotice(AUDIO_NOTICE_NONE); // a card is there again
    }
    return true;
}

static bool cancelWanted = false; // discard what was recorded

static void finishRecording() {
    size_t length = scratchLength;
    if (cancelWanted) length = 0; // too short to keep, below
    if (streamingRecord) {
        state = AUDIO_SAVING;
        uint32_t start = millis();
        Moment moment = streamMoment;
        moment.micGainDb = gainNow;
        moment.micGainAuto = gainSetting == GAIN_AUTO;
        analyseEnvelope(moment, length);
        bool ok = storageStreamEnd(moment); // sets length and dataOffset; false when nothing of it is on the card
        bool cardFailed = storageStreamFailed();
        free(envelope);
        envelope = nullptr;
        streamingRecord = false;
        if (!ok || moment.length < MIN_MOMENT_SAMPLES || cancelWanted) {
            if (ok) storageDeleteMoment(moment);
            state = AUDIO_IDLE;
            if (cancelWanted) {
                Serial.println("Recording cancelled");
            } else if (!cardFailed) {
                Serial.println("Recording too short, discarded");
            } else {
                Serial.println("Recording lost: the SD card didn't take it");
                setNotice(AUDIO_NOTICE_NOT_SAVED);
            }
        } else {
            nextMomentNumber = moment.number + 1;
            xSemaphoreTake(momentLock, portMAX_DELAY);
            memmove(&moments[1], &moments[0], momentTotal * sizeof(Moment));
            moments[0] = moment;
            momentTotal++;
            listVersion++;
            xSemaphoreGive(momentLock);
            state = AUDIO_IDLE;
            Serial.printf("Saved Moment %d to SD card as %s/%s in %lu ms: %.2f s, playback gain %.1fx, mic gain %d dB%s, %u KB RAM free\n",
                          moment.number, moment.folder, moment.fileName, millis() - start, (float)moment.length / SAMPLE_RATE,
                          moment.gain, gainNow, gainSetting == GAIN_AUTO ? (agcChanges ? " (auto, adjusted)" : " (auto)") : "",
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
            if (cardFailed) {
                Serial.println("[audio] the SD card stopped taking the recording: it ends there, and what was written is kept");
                setNotice(AUDIO_NOTICE_CARD_FAILED);
            }
        }
        // A card that failed may be full, or out of its slot: asked now, so
        // that the list never shows a card that doesn't answer
        if (cardFailed) recheckCard();
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
    moment.sampleRate = SAMPLE_RATE;
    moment.recordedAt = recordStartedAt;
    moment.startMs = recordStartedMs;
    moment.length = length;
    moment.micGainDb = gainNow;
    moment.micGainAuto = gainSetting == GAIN_AUTO;
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
    // The card was found gone when this recording started, and the screen
    // was held still since: the line that says so counts its seconds from
    // now, when the list comes back and it can be read
    if (!onCard && notice == AUDIO_NOTICE_NO_CARD) setNotice(AUDIO_NOTICE_NO_CARD);
    state = AUDIO_IDLE;
    Serial.printf("Saved Moment %d %s%s%s%s in %lu ms: %.2f s, playback gain %.1fx, mic gain %d dB%s\n",
                  moment.number, onCard ? "to SD card as " : "in memory", onCard ? moment.folder : "", onCard ? "/" : "",
                  moment.fileName, millis() - start,
                  (float)length / SAMPLE_RATE, moment.gain, gainNow,
                  gainSetting == GAIN_AUTO ? (agcChanges ? " (auto, adjusted)" : " (auto)") : "");
}

static void startPlayback(int index) {
    if (index < 0 || index >= momentTotal) return;
    playPos = 0;
    speed = 1;
    stepBefore = (float)(moments[index].sampleRate ? moments[index].sampleRate : SAMPLE_RATE) / SAMPLE_RATE;
    speedDirection = 0;
    returning = false;
    restoreSpeed = false;
    waitedAtStart = 0;
    underruns = 0;
    fileEnd = moments[index].length;
    if (!moments[index].samples) {
        int number = moments[index].number;
        File file = readerTaskHandle ? storageOpenMoment(moments[index]) : File();
        if (!file) {
            Serial.printf("Can't open Moment %d on the SD card\n", number);
            recheckCard(); // taken out and put back, perhaps: the list is read again then
            return;
        }
        // The reader takes the file, resets the window and loads the first half second; wait for it
        xSemaphoreTake(firstBlock, 0);
        xSemaphoreTake(momentLock, portMAX_DELAY);
        nextFile = file;
        file = File();
        xSemaphoreGive(momentLock);
        readerMoment = index;
        readerSeq++;
        xTaskNotifyGive(readerTaskHandle);
        if (xSemaphoreTake(firstBlock, pdMS_TO_TICKS(400)) != pdTRUE) {
            Serial.printf("Can't read Moment %d from the SD card\n", number);
            stopPlayback(); // the reader lets go of the file
            recheckCard();
            return;
        }
    }
    playingIndex = index;
    digitalWrite(PA_ENABLE_PIN, HIGH);
    state = AUDIO_PLAYING;
    Serial.printf("Playing Moment %d\n", moments[index].number);
}

static void stopPlayback() {
    digitalWrite(PA_ENABLE_PIN, LOW);
    if (underruns) Serial.printf("[play] %lu chunks waited on the card\n", (unsigned long)underruns);
    bool onCard = readerMoment >= 0;
    if (readerTaskHandle) { // without the reader nothing plays from the card, and there is nobody to tell
        xSemaphoreTake(readerIdle, 0);
        readerMoment = -1;
        readerSeq++;
        xTaskNotifyGive(readerTaskHandle);
        if (onCard && xSemaphoreTake(readerIdle, pdMS_TO_TICKS(600)) != pdTRUE) {
            Serial.println("[play] the reader didn't let go of the file"); // a block read takes tens of ms; this shouldn't happen
        }
    }
    playingIndex = -1;
    state = AUDIO_IDLE;
}

static void deleteMoment(int index) {
    if (index < 0 || index >= momentTotal) return;
    if (playingIndex == index) stopPlayback();
    int number = moments[index].number;
    if (moments[index].samples) {
        free(moments[index].samples);
    } else if (!storageDeleteMoment(moments[index])) {
        // The file is still there, or the card no longer answers: the moment
        // is not gone either way, and the list must not say that it is
        Serial.printf("Couldn't delete Moment %d from the SD card\n", number);
        recheckCard();
        return;
    }

    xSemaphoreTake(momentLock, portMAX_DELAY);
    memmove(&moments[index], &moments[index + 1], (momentTotal - index - 1) * sizeof(Moment));
    momentTotal--;
    if (playingIndex > index) playingIndex--;
    if (readerMoment > index) readerMoment--; // the reader has its own copy of the moment; only the index moves
    listVersion++;
    xSemaphoreGive(momentLock);
    Serial.printf("Deleted Moment %d\n", number);
}

static void tagMoment(int index, int tag) {
    if (index < 0 || index >= momentTotal || tag < 0 || tag > CATEGORY_MAX) return;
    xSemaphoreTake(momentLock, portMAX_DELAY);
    moments[index].tag = tag;
    listVersion++;
    xSemaphoreGive(momentLock);
    if (!moments[index].samples) {
        Moment updated = moments[index];
        if (!storageUpdateMoment(updated)) {
            Serial.printf("Couldn't save the tag of Moment %d to the SD card\n", moments[index].number);
            recheckCard(); // if the card was changed, the list is read again and shows what its files say
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
    if (storageAvailable() && notice == AUDIO_NOTICE_NO_CARD) setNotice(AUDIO_NOTICE_NONE); // a card is there again
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

// Where the moment with this number is in the list now, -1 if it isn't
static int indexOfNumber(int number) {
    for (int i = 0; i < momentTotal; i++) {
        if (moments[i].number == number) return i;
    }
    return -1;
}

static void handleCommand(const Command &cmd) {
    switch (cmd.type) {
    case CMD_RECORD_START:
        if (state == AUDIO_PLAYING) stopPlayback();
        if (state == AUDIO_IDLE) {
            uint32_t seconds = cmd.arg > 0 ? cmd.arg : MAX_RECORDING_SECONDS;
            setNotice(AUDIO_NOTICE_NONE);
            // With the codec off, time has passed: enough for the card to have been
            // taken out or put in. It is asked once, here or below.
            bool cardAsked = false;
            if (recordableSamples() < MIN_MOMENT_SAMPLES) {
                // No room as far as is known, which only another card could change. Said
                // before the state changes, so that the recording screen doesn't flash up.
                if (!running) {
                    recheckCard();
                    cardAsked = true;
                }
                if (recordableSamples() < MIN_MOMENT_SAMPLES) {
                    Serial.println("No room for another recording");
                    break;
                }
            }
            // Recording from here on, for everyone who asks: the schedule in main.cpp gives
            // a recording it requested one second to show and takes it as over if it doesn't,
            // and getting the card ready can take longer than that. The recording screen is
            // drawn once, at this change, with the length and limit as they are now.
            scratchLength = 0;
            recordLimit = (size_t)min((uint64_t)seconds * SAMPLE_RATE, (uint64_t)recordableSamples());
            state = AUDIO_RECORDING;
            if (!running && !cardAsked) recheckCard();
            bool awake = audioWake();
            if (!awake) setNotice(AUDIO_NOTICE_NOT_SAVED);
            if (!awake || !startRecording(seconds)) state = AUDIO_IDLE;
        }
        break;
    case CMD_RECORD_STOP:
        if (state == AUDIO_RECORDING) finishRecording();
        break;
    case CMD_RECORD_CANCEL:
        if (state == AUDIO_RECORDING) {
            cancelWanted = true;
            finishRecording();
            cancelWanted = false;
        }
        break;
    case CMD_PLAY:
        if (state == AUDIO_PLAYING) stopPlayback();
        if (state == AUDIO_IDLE) {
            if (!running) recheckCard(); // as for a recording
            if (audioWake()) startPlayback(indexOfNumber(cmd.arg));
        }
        break;
    case CMD_STOP_PLAYBACK:
        if (state == AUDIO_PLAYING) stopPlayback();
        break;
    case CMD_DELETE:
        // Taking a file off the card keeps this task busy for longer than
        // the microphone's buffers hold: the recording would lose that sound
        if (state == AUDIO_RECORDING) Serial.printf("[audio] Moment %d is not deleted while a recording runs\n", cmd.arg);
        else deleteMoment(indexOfNumber(cmd.arg));
        break;
    case CMD_SELF_TEST:
        if (state == AUDIO_IDLE && audioWake()) startSelfTest();
        break;
    case CMD_RECHECK_CARD:
        // Only with nothing going on: asking stops a playback, and a card
        // that is recorded onto is not to be started afresh
        if (state == AUDIO_IDLE) recheckCard();
        break;
    case CMD_SUSPEND:
        if (state == AUDIO_RECORDING) finishRecording();
        if (state == AUDIO_PLAYING) stopPlayback();
        state = AUDIO_IDLE;
        audioStandby();
        xSemaphoreGive(suspendDone);
        break;
    case CMD_TAG:
        if (state == AUDIO_RECORDING) Serial.printf("[audio] Moment %d is not tagged while a recording runs\n", cmd.arg); // as for deleting
        else tagMoment(indexOfNumber(cmd.arg), cmd.arg2);
        break;
    case CMD_TAGS_ABOVE: // a removed category: its moments lose the tag, files renamed
        for (int i = 0; i < momentTotal; i++) {
            if (moments[i].tag > cmd.arg) tagMoment(i, TAG_NONE);
        }
        break;
    case CMD_FORMAT:
        if (state == AUDIO_IDLE || state == AUDIO_PLAYING) formatCard();
        break;
    case CMD_GAIN:
        gainSetting = cmd.arg;
        if (cmd.arg != GAIN_AUTO) gainNow = cmd.arg;
        if (running) applyGain(gainNow); // else when the codec next powers up
        break;
    case CMD_USB_MIC:
        if (state == AUDIO_PLAYING) stopPlayback();
        if (state == AUDIO_IDLE && audioWake()) state = AUDIO_USB_MIC;
        break;
    }
}

// One sample of the playing moment: from memory, or from the ring. Outside
// the loaded window it's silence, and the reader is nudged.
static int16_t sampleAt(const Moment &moment, size_t i, bool &missed) {
    if (moment.samples) return moment.samples[i];
    size_t ws = winStart, we = winEnd;
    if (i < ws || i >= we) {
        missed = true;
        return 0;
    }
    return ring[i % RING_SAMPLES];
}

// --- The reader task: keeps the ring loaded around the playhead ---

// `want` samples from `from` into the ring, which may wrap; how many came
static size_t readIntoRing(uint32_t dataOffset, size_t from, size_t want) {
    if (!playFile.seek(dataOffset + from * sizeof(int16_t))) return 0;
    size_t off = from % RING_SAMPLES, first = min(want, RING_SAMPLES - off);
    size_t n = playFile.read((uint8_t *)(ring + off), first * sizeof(int16_t)) / sizeof(int16_t);
    if (n < first || want == first) return n;
    return n + playFile.read((uint8_t *)ring, (want - first) * sizeof(int16_t)) / sizeof(int16_t);
}

static void readerTask(void *) {
    int current = -1;
    uint32_t seq = 0;
    uint32_t dataOffset = 0; // of the moment served: its own copy, since the list may shift under it
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
        uint32_t s = readerSeq;
        if (s != seq) { // a start or a stop
            seq = s;
            if (playFile) playFile.close();
            current = readerMoment;
            File handed;
            xSemaphoreTake(momentLock, portMAX_DELAY);
            if (current >= 0) dataOffset = moments[current].dataOffset;
            handed = nextFile;
            nextFile = File();
            xSemaphoreGive(momentLock);
            if (current >= 0 && handed) {
                playFile = handed;
                winStart = winEnd = 0;
            } else {
                if (handed) handed.close(); // opened for a start that was given up since
                current = -1;
                xSemaphoreGive(readerIdle); // the file is let go of
            }
        }
        if (current < 0) continue;
        double pos = playPos;
        size_t p = pos < 0 ? 0 : (size_t)pos;
        size_t ws = winStart, we = winEnd, end = fileEnd;
        if (p < ws || p > we) { // the playhead left the window: start afresh at it
            winStart = winEnd = ws = we = p;
        }
        // Ahead: always a little, and two seconds when running forward
        float sp = speed;
        size_t ahead = sp >= 0 ? AHEAD_SAMPLES : 4 * CHUNK_FRAMES;
        if (we < end && we < p + ahead) {
            size_t want = min(READ_BLOCK, end - we);
            if (we + want > ws + RING_SAMPLES) winStart = we + want - RING_SAMPLES; // the back is overwritten: let go of it first
            size_t n = readIntoRing(dataOffset, we, want);
            if (readerSeq != seq) continue; // the playback changed under the read: nothing of it counts
            if (n < want) fileEnd = we + n; // shorter than its header says
            winEnd = we + n;
            if (ws == we && n > 0) xSemaphoreGive(firstBlock);
            continue; // one block per turn, then look again
        }
        // Behind: two seconds when running backwards
        if (sp < 0 && ws > 0 && ws + AHEAD_SAMPLES > p) {
            size_t want = min(READ_BLOCK, ws), from = ws - want;
            if (we > from + RING_SAMPLES) winEnd = from + RING_SAMPLES; // the front goes first
            size_t n = readIntoRing(dataOffset, from, want);
            if (readerSeq != seq) continue;
            if (n == want) winStart = from;
        }
    }
}

static void updateSpeed() {
    int direction = speedDirection;
    float s = speed;
    if (direction == 0 && restoreSpeed) {
        s = speedAtPress; // a tap moved the speed a little before it was known as a tap: undo that
        restoreSpeed = false;
    }
    if (direction != 0) {
        // Towards the held direction: by steps away from zero, straight through
        // zero inside the scrub band, like a reel slowed to a stop and turned
        float towards = direction > 0 ? 1 : -1;
        int ramp = constrain((int)rampSetting, 0, RAMP_COUNT - 1);
        float top = maxSpeedSetting;
        if (s * towards >= SCRUB_SPEED) s *= SPEED_STEP[ramp];          // same way: faster
        else if (s * towards <= -SCRUB_SPEED) s /= SPEED_STEP[ramp];    // other way: slower
        else s += towards * SCRUB_STEP[ramp];                           // through zero
        s = constrain(s, -top, top);
        returning = false;
    } else if (holdSetting && !returning) {
        // Let go: the speed stays, until a tap
    } else if (fabsf(s - 1) > 0.01f) {
        s = 1 + (s - 1) * SPEED_RETURN[constrain((int)rampSetting, 0, RAMP_COUNT - 1)]; // let go: back to normal, forward
    } else {
        s = 1;
        returning = false;
    }
    speed = s;
}

// Fills `out` (stereo) with up to `frames` frames of the playing moment at the
// current speed, by linear interpolation; fewer at the end. The speed glides
// from the last chunk's to this one's across the chunk, so a ramp has no steps.
static volatile uint32_t playRate = SAMPLE_RATE; // the playing moment's

static size_t playbackChunk(const Moment &moment, int16_t *out, size_t frames) {
    updateSpeed();
    double pos = playPos;
    playRate = moment.sampleRate ? moment.sampleRate : SAMPLE_RATE;
    float stepAfter = speed * (float)playRate / SAMPLE_RATE; // a 48 kHz moment steps half as fast
    size_t length = min(moment.length, (size_t)fileEnd);
    size_t n = 0;
    bool missed = false;
    while (n < frames && length >= 2) {
        float step = stepBefore + (stepAfter - stepBefore) * (n + 1) / frames;
        if (pos < 0) pos = 0; // backwards to the start: wait there
        size_t i = (size_t)pos;
        if (i + 1 >= length) {
            if (step < 0) i = length - 2; // at the end but running back: carry on from there
            else break;
        }
        float frac = (float)(pos - i);
        float s = (sampleAt(moment, i, missed) * (1 - frac) + sampleAt(moment, i + 1, missed) * frac) * moment.gain;
        out[2 * n] = out[2 * n + 1] = (int16_t)constrain(s, -32768.0f, 32767.0f);
        pos += step;
        n++;
    }
    stepBefore = stepAfter;
    playPos = pos;
    if (missed) {
        underruns++;
        xTaskNotifyGive(readerTaskHandle);
    }
    // Left running backwards by itself (the speed stays and nothing is held), a
    // moment that has reached its start waits there for a few seconds, in which
    // a tap or a push up plays it forwards again, and then ends as it does at
    // its other end. Without this it stood at the start for good, playing
    // nothing, and kept the device from going to sleep.
    if (pos < 0 && stepAfter < 0 && speedDirection == 0 && holdSetting && !returning) {
        if (++waitedAtStart >= START_WAIT_CHUNKS) return 0;
    } else {
        waitedAtStart = 0;
    }
    return n;
}

static void audioTask(void *) {
    static int16_t in[CHUNK_FRAMES * 2];
    static int16_t out[CHUNK_FRAMES * 2];

    for (;;) {
        Command cmd;
        if (!running) {
            // The request stays in the queue while it is carried out, so that
            // audioAsleep() says no for as long as the work lasts. Waking the
            // codec for a playback takes some 30 ms with the state still idle,
            // and the main loop, seeing nothing going on, sent the chip into
            // light sleep in the middle of it: on battery a press on a moment
            // played nothing until the next key or the next minute.
            if (xQueuePeek(commandQueue, &cmd, portMAX_DELAY) == pdTRUE) {
                handleCommand(cmd);
                xQueueReceive(commandQueue, &cmd, 0); // the same one: only this task takes from the queue
            }
            idleSince = millis();
            continue;
        }

        // Reading paces the loop: blocks until the next 16 ms of mic audio arrives
        size_t bytesRead = 0;
        i2s_read(I2S_NUM_0, in, sizeof(in), &bytesRead, portMAX_DELAY);
        size_t frames = bytesRead / 4;

        freshStart = false;
        while (xQueueReceive(commandQueue, &cmd, 0) == pdTRUE) handleCommand(cmd);
        if (freshStart) continue; // the chunk in hand is from before the recording was set up: it starts with the next

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
                if (streamMoment.recordedAt) { // a file on the card, named by the clock a moment ago: it gets the exact time at its end
                    streamMoment.recordedAt = recordStartedAt;
                    streamMoment.startMs = recordStartedMs;
                }
            }
            if (streamingRecord) {
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
            // At its limit, or as soon as the card has stopped taking it: nothing after that would be kept
            if (scratchLength >= recordLimit || (streamingRecord && storageStreamFailed())) finishRecording();
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
        } else if (state == AUDIO_USB_MIC) {
            static int16_t mono[CHUNK_FRAMES];
            static bool watched = false;
            if (!watched) {
                watched = true;
                usbMicWatch();
            }
            size_t n = frames - skip;
            for (size_t i = 0; i < n; i++) mono[i] = in[2 * (skip + i)]; // ES8311 ADC is on the left slot
            usbMicFeed(mono, n);
            if (gainSetting == GAIN_AUTO) autoGain(in, frames, skip);
        }

        if (state != AUDIO_IDLE) idleSince = millis();
        else if (millis() - idleSince > STANDBY_AFTER_MS && uxQueueMessagesWaiting(commandQueue) == 0) audioStandby();
    }
}

static bool codecInit() {
    if (!codec) codec = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
    if (!codec) return false;
    // Once a start, before anything is set: the codec has kept its registers through the reset, and
    // through a flashing too, and es8311_init() writes only some of them. Waveshare's factory
    // firmware leaves the DAC muted: flashed over it, the speaker stayed silent until the board
    // lost its power.
    static bool ownRegisters = false;
    if (!ownRegisters) {
        int changed = 0;
        if (es8311_registers_default(codec, &changed) != ESP_OK) return false;
        ownRegisters = true;
        if (changed) Serial.printf("[audio] codec: %d registers held another firmware's values, set back to the chip's own\n", changed);
    }
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
    es8311_microphone_fade(codec, FINE_FADE); // later changes of that volume glide
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
    // A DMA buffer holds 1024 frames at most; at 96 kHz a chunk is two of them.
    // These live in internal RAM, which the Wi-Fi driver needs too: 8 of them
    // (64 ms at 96 kHz) are 24 KB, as at 48 kHz
    cfg.dma_buf_count = 8;
    cfg.dma_buf_len = CHUNK_FRAMES > 1024 ? CHUNK_FRAMES / 2 : CHUNK_FRAMES;
    cfg.tx_desc_auto_clear = true; // output silence when nothing is being played
    cfg.mclk_multiple = MCLK_MULTIPLE == 128 ? I2S_MCLK_MULTIPLE_128 : I2S_MCLK_MULTIPLE_256;

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
    maxSpeedSetting = prefs.getInt("vmax", 3);
    rampSetting = constrain(prefs.getInt("vramp", RAMP_NORMAL), 0, RAMP_COUNT - 1);
    holdSetting = prefs.getInt("vhold", 0) != 0;
    if (gainSetting != GAIN_AUTO && gainSetting < MIN_GAIN_DB) gainSetting = MIN_GAIN_DB; // saved by a firmware that allowed less
    prefs.end();

    pinMode(PA_ENABLE_PIN, OUTPUT);
    digitalWrite(PA_ENABLE_PIN, LOW);

    if (storageInit()) loadCardMoments();

    // Start I2S first so the codec sees MCLK while it is configured
    if (!i2sInit()) {
        Serial.println("I2S init failed");
        return false;
    }
    if (!codecInit() && !codecInit()) {
        Serial.println("ES8311 codec init failed");
        return false;
    }
    running = true;
    idleSince = millis();
    ring = (int16_t *)ps_malloc(RING_SAMPLES * sizeof(int16_t));
    firstBlock = xSemaphoreCreateBinary();
    readerIdle = xSemaphoreCreateBinary();
    // The reader only seeks and reads (see nextFile), which leaves about 1 KB
    // of these 4 KB free even while the card driver reports an error
    if (!ring || !firstBlock || !readerIdle ||
        xTaskCreatePinnedToCore(readerTask, "reader", 4096, NULL, 2, &readerTaskHandle, 1) != pdPASS) { // core 1, with the SD writer
        readerTaskHandle = nullptr;
        Serial.println("Can't set up playback from the SD card");
    }
    // 8 KB: saving builds the WAV header and card paths on this stack, and the
    // card's long-file-name code adds its own buffers
    if (!commandQueue || !suspendDone || xTaskCreatePinnedToCore(audioTask, "audio", 8192, NULL, 5, NULL, 0) != pdPASS) {
        Serial.println("Can't start the audio task");
        audioStandby(); // nobody will use the codec
        return false;
    }
    ready = true;
    return true;
}

// Without the audio task a request would sit in the queue for good, and keep
// the chip from its light sleep: it is dropped
static void sendCommand(CommandType type, int arg = 0, int arg2 = 0) {
    if (!ready) return;
    Command cmd = {type, arg, arg2};
    xQueueSend(commandQueue, &cmd, 0);
}

// The number of the moment at this place in the list, as the caller sees it now
static bool numberAt(int index, int &number) {
    bool found = false;
    xSemaphoreTake(momentLock, portMAX_DELAY);
    if (index >= 0 && index < momentTotal) {
        number = moments[index].number;
        found = true;
    }
    xSemaphoreGive(momentLock);
    return found;
}

void audioStartRecording(uint32_t seconds) { sendCommand(CMD_RECORD_START, seconds); }
void audioStopRecording() { sendCommand(CMD_RECORD_STOP); }
void audioCancelRecording() { sendCommand(CMD_RECORD_CANCEL); }
void audioPlay(int index) {
    int number;
    if (numberAt(index, number)) sendCommand(CMD_PLAY, number);
}
void audioStopPlayback() { sendCommand(CMD_STOP_PLAYBACK); }
void audioDelete(int index) {
    int number;
    if (numberAt(index, number)) sendCommand(CMD_DELETE, number);
}
void audioDeleteNumber(int number) { sendCommand(CMD_DELETE, number); }
void audioRecheckCard() { sendCommand(CMD_RECHECK_CARD); }
void audioSelfTest() { sendCommand(CMD_SELF_TEST); }
void audioStartUsbMic() { sendCommand(CMD_USB_MIC); }
void audioSetTag(int index, int tag) {
    int number;
    if (numberAt(index, number)) sendCommand(CMD_TAG, number, tag);
}
void audioClearTagsAbove(int count) { sendCommand(CMD_TAGS_ABOVE, count); }
void audioSpeedHold(int direction) {
    if (direction != 0 && speedDirection == 0) speedAtPress = speed;
    speedDirection = direction;
}
void audioSpeedTap() {
    restoreSpeed = true;
    returning = true;
}

void audioSetVarispeed(int maxSpeed, int ramp, int hold) {
    if (maxSpeed > 0) maxSpeedSetting = constrain(maxSpeed, 1, 8);
    if (ramp >= 0) rampSetting = constrain(ramp, 0, RAMP_COUNT - 1);
    if (hold >= 0) holdSetting = hold != 0;
    Preferences prefs;
    prefs.begin("zvukopis", false);
    prefs.putInt("vmax", maxSpeedSetting);
    prefs.putInt("vramp", rampSetting);
    prefs.putInt("vhold", holdSetting ? 1 : 0);
    prefs.end();
}

int audioVarispeedMax() { return maxSpeedSetting; }
int audioVarispeedRamp() { return rampSetting; }
bool audioVarispeedHold() { return holdSetting; }

const char *varispeedRampName(int ramp) {
    static const char *const names[RAMP_COUNT] = {"gentle", "normal", "quick"};
    return names[constrain(ramp, 0, RAMP_COUNT - 1)];
}

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
    formatResult = ready ? 0 : -1; // with nobody to do it, the answer is known at once
    sendCommand(CMD_FORMAT);
}

int audioFormatResult() { return formatResult; }

void audioSuspend() {
    if (!ready) return; // nothing runs that could be suspended, and nobody would answer
    xSemaphoreTake(suspendDone, 0);
    sendCommand(CMD_SUSPEND);
    // Waits for the audio task, which may be saving a long recording first
    if (xSemaphoreTake(suspendDone, pdMS_TO_TICKS(15000)) != pdTRUE) Serial.println("Audio suspend timed out");
}

bool audioAsleep() { return !running && uxQueueMessagesWaiting(commandQueue) == 0; }

AudioState audioState() { return state; }

bool audioReady() { return ready; }
bool audioListFull() { return momentTotal >= MAX_MOMENTS; }
AudioNotice audioNotice(uint32_t &since) {
    since = noticeSince;
    return notice;
}

float audioRecordingSeconds() {
    return state == AUDIO_RECORDING || state == AUDIO_SAVING ? (float)scratchLength / SAMPLE_RATE : 0;
}

float audioRecordingLimitSeconds() { return (float)recordLimit / SAMPLE_RATE; }

int audioPlayingIndex() { return playingIndex; }

float audioPlaybackSeconds() {
    return state == AUDIO_PLAYING ? (float)(playPos / playRate) : 0;
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
