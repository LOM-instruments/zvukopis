#include "spectrum.h"

#include <Arduino.h>
#include <dsps_fft2r.h>
#include <math.h>

#include "audio.h"
#include "storage.h"

static const int FFT_SIZE = SAMPLE_RATE >= 96000 ? 4096 : 2048; // 23 Hz bins either way
// The loudness of a cell lies between -120 dB (the 1e-12 added before the
// logarithm) and 67 dB (what 4096 windowed samples at full scale can add up
// to in one bin)
static const float LOUDNESS_FROM_DB = -130;
static const int LOUDNESS_STEPS = 400; // of half a dB: up to 70 dB

static Spectrogram result;
static float *dbGrid = nullptr; // kept between analyses (PSRAM), grown when a request is bigger
static size_t dbGridCells = 0;
static float *averageDb = nullptr; // FFT_SIZE / 2
static TaskHandle_t task = nullptr;
// Held by the screen while it draws from `result` (spectrumHold). The task
// waits for it before a pass writes into the grid, so that no frame is drawn
// half from one analysis and half from the next.
static SemaphoreHandle_t resultLock = nullptr;

// What is asked for. Only spectrumRequest() writes these; the task reads them.
// The grid: its width (12 bits), its height (12 bits) and the zoom of its
// frequency scale (8 bits) in one word, so that the task never takes the
// width of one request with the height or the zoom of another. It changes
// when the button guide is shown or hidden, and when another scale is chosen.
// The stamp (momentStamp) says which recording the number stood for: numbers
// are a card's own, and another card can hold another recording under the
// same one.
static volatile int wantedNumber = -1;
static volatile uint32_t wantedGrid = 0, wantedStamp = 0;
// What `result` holds. Only the task writes these.
static volatile int readyNumber = -1;
static volatile uint32_t readyGrid = 0, readyStamp = 0;
// The pass under way, or the last one, which `progress` counts for: another
// request for the same moment (another scale) starts from 0, not from where
// the pass before it had got to
static volatile int passNumber = -1;
static volatile uint32_t passGrid = 0, passStamp = 0;
static volatile int progress = 0; // percent of its columns done

enum Outcome { DONE, SUPERSEDED, FAILED, UNREADABLE };
// The last pass that ended without a picture: what it was for, and whether
// for lack of memory (FAILED) or of the recording itself (UNREADABLE).
// Asking for the same again tries again.
static volatile int failedNumber = -1;
static volatile uint32_t failedGrid = 0, failedStamp = 0;
static volatile Outcome failedWhy = FAILED;

static const int GRID_MAX_SIDE = 4095;
static_assert(SPECTRUM_MAX_ZOOM <= 255, "the zoom has eight bits of the grid word");

static uint32_t gridWord(int width, int height, int zoom) {
    return (uint32_t)constrain(width, 1, GRID_MAX_SIDE) << 20 | (uint32_t)constrain(height, 1, GRID_MAX_SIDE) << 8 |
           (uint32_t)constrain(zoom, 1, SPECTRUM_MAX_ZOOM);
}
static int gridWidth(uint32_t grid) { return grid >> 20; }
static int gridHeight(uint32_t grid) { return grid >> 8 & 0xFFF; }
static float gridHighHz(uint32_t grid) { return SPECTRUM_HIGH_HZ / (grid & 0xFF); }

float spectrumRowHz(int row, int height, float highHz) {
    return highHz * (1 - (row + 0.5f) / height); // row 0 at the top
}

int spectrumHzRow(float hz, int height, float highHz) {
    return constrain((int)((1 - hz / highHz) * height), 0, height - 1);
}

// Reads `count` samples from `start` (zeros past the end). False when a
// moment on the card could not be read: the card has gone, or its file has.
class SampleSource {
  public:
    explicit SampleSource(const Moment &moment) : moment(moment), mounts(storageMounts()) {
        if (!moment.samples) file = storageOpenMoment(moment);
    }
    ~SampleSource() {
        if (!file) return;
        // After another mount the file's descriptor may be another file's by
        // now, a recording's even: then it is left alone, never closed
        if (storageMounts() == mounts) file.close();
        else (void)new File(file);
    }
    bool read(long start, int count, int16_t *out) {
        for (int i = 0; i < count; i++) out[i] = 0;
        long from = max(0L, start), to = min((long)moment.length, start + count);
        if (to <= from) return true;
        if (moment.samples) {
            memcpy(out + (from - start), moment.samples + from, (to - from) * sizeof(int16_t));
            return true;
        }
        if (!file || storageMounts() != mounts) return false; // no file, or not this mount's any more: hands off it
        size_t bytes = (to - from) * sizeof(int16_t);
        return file.seek(moment.dataOffset + from * sizeof(int16_t)) && file.read((uint8_t *)(out + (from - start)), bytes) == bytes;
    }

  private:
    const Moment &moment;
    const uint32_t mounts; // storageMounts() when the file was opened
    File file;
};

static bool findMoment(int number, Moment &moment) {
    for (int i = 0; momentGet(i, moment); i++) {
        if (moment.number == number) return true;
    }
    return false;
}

// Tells one recording from another under the same moment number, by what a
// recording keeps for good (so not by its tag or its file's name). Never 0,
// which stands for no moment at all.
static uint32_t momentStamp(const Moment &moment) {
    uint32_t hash = 2166136261u; // FNV-1a
    auto mix = [&hash](const void *data, size_t length) {
        for (size_t i = 0; i < length; i++) hash = (hash ^ ((const uint8_t *)data)[i]) * 16777619u;
    };
    mix(&moment.samples, sizeof(moment.samples)); // where it is held in memory, nullptr on the card
    mix(&moment.recordedAt, sizeof(moment.recordedAt));
    mix(&moment.startMs, sizeof(moment.startMs));
    mix(&moment.length, sizeof(moment.length));
    mix(&moment.sampleRate, sizeof(moment.sampleRate));
    mix(moment.waveform, sizeof(moment.waveform));
    return hash ? hash : 1;
}

// Another moment, another grid, or another recording under that number has been asked for meanwhile
static bool superseded(int number, uint32_t grid, uint32_t stamp) {
    return wantedNumber != number || wantedGrid != grid || wantedStamp != stamp;
}

// Works the moment's spectrogram out into `result`. `stamp` comes back as
// that of the recording it read.
static Outcome compute(int number, uint32_t grid, uint32_t &stamp) {
    const int width = gridWidth(grid), height = gridHeight(grid);
    const float highHz = gridHighHz(grid);
    progress = 0; // not the figure the pass before this one ended on
    passNumber = -1;
    Moment moment;
    bool found = findMoment(number, moment);
    stamp = found ? momentStamp(moment) : 0;
    passStamp = stamp;
    passGrid = grid;
    passNumber = number;
    // The list has changed since this was asked for: the number is another
    // recording's now. The screen asks again when it sees the list change.
    if (superseded(number, grid, stamp)) return SUPERSEDED;
    if (!found || moment.length < FFT_SIZE / 4) return FAILED;
    uint32_t start = millis();

    if (!dbGrid || (size_t)width * height > dbGridCells) {
        free(dbGrid);
        dbGridCells = (size_t)width * height;
        dbGrid = (float *)ps_malloc(dbGridCells * sizeof(float));
        if (!dbGrid) dbGridCells = 0;
    }
    float *db = dbGrid;
    float *fft = (float *)heap_caps_aligned_alloc(16, FFT_SIZE * 2 * sizeof(float), MALLOC_CAP_INTERNAL);
    int16_t *frame = (int16_t *)malloc(FFT_SIZE * sizeof(int16_t));
    float *window = (float *)malloc(FFT_SIZE * sizeof(float));
    double *binPower = (double *)ps_calloc(FFT_SIZE / 2, sizeof(double));
    // Bin range of each row: the bins inside its slice of the linear scale
    int *binLow = (int *)malloc(height * sizeof(int)), *binHigh = (int *)malloc(height * sizeof(int));
    // How many cells are how loud, in steps of half a dB: for brightDb and quietDb
    uint32_t *loudnessCount = (uint32_t *)calloc(LOUDNESS_STEPS, sizeof(uint32_t));
    bool ok = db && fft && frame && window && binPower && binLow && binHigh && loudnessCount;
    if (ok && !averageDb) averageDb = (float *)ps_malloc(FFT_SIZE / 2 * sizeof(float));
    ok = ok && averageDb;
    Outcome outcome = ok ? DONE : FAILED;
    if (!ok) {
        Serial.printf("[spectrum] no memory for the analysis of Moment %d (%u KB of PSRAM free)\n", number,
                      (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }
    if (ok) {
        const float rate = moment.sampleRate ? moment.sampleRate : SAMPLE_RATE;
        const float binHz = rate / FFT_SIZE;
        // The levels (the loudest point, brightDb, quietDb) are taken from
        // this bin up (not from the rumble below PEAK_MIN_HZ), the same on
        // every scale and at every height of the grid
        const int firstBin = max(1, (int)ceilf(PEAK_MIN_HZ / binHz));
        // brightDb and quietDb from the rows under this only, which every scale has
        const float levelsHighHz = SPECTRUM_HIGH_HZ / SPECTRUM_LEVELS_ZOOM;
        int levelFrom = height, levelTo = -1, levelRows = 0; // those rows: they lie together
        for (int r = 0; r < height; r++) {
            float upper = highHz * (1 - (float)r / height);
            float lower = highHz * (1 - (float)(r + 1) / height);
            int low = (int)ceilf(lower / binHz), high = (int)floorf(upper / binHz);
            // On a zoomed scale a row can be narrower than a bin and hold
            // none: it shows the bin nearest to its middle
            if (high < low) low = high = (int)roundf((lower + upper) / 2 / binHz);
            binLow[r] = max(1, low); // bin 0 is DC
            binHigh[r] = max(binLow[r], high);
            // No part for a row of rumble only, nor for one past what the
            // recording's sample rate reaches, which is empty
            if (binHigh[r] >= firstBin && binLow[r] < FFT_SIZE / 2 && lower < levelsHighHz) {
                levelFrom = min(levelFrom, r);
                levelTo = r;
                levelRows++;
            }
        }
        // Hann window
        for (int i = 0; i < FFT_SIZE; i++) window[i] = 0.5f - 0.5f * cosf(2 * (float)M_PI * i / (FFT_SIZE - 1));
        dsps_fft2r_init_fc32(NULL, FFT_SIZE);

        SampleSource source(moment);
        float loudest = -1000;
        for (int c = 0; c < width && !superseded(number, grid, stamp); c++) {
            progress = c * 100 / width;
            long centre = (long)((c + 0.5) * moment.length / width);
            if (!source.read(centre - FFT_SIZE / 2, FFT_SIZE, frame)) {
                // Zeros in its place would give a picture of silence, kept as this moment's
                Serial.printf("[spectrum] Moment %d could not be read: no analysis\n", number);
                outcome = UNREADABLE;
                break;
            }
            // The ADC's residual offset is tiny (about -100 dBFS) but far above
            // one bin's share of the noise floor, so it must not leak into
            // the first bins through the window: take the frame's mean out
            int32_t sum = 0;
            for (int i = 0; i < FFT_SIZE; i++) sum += frame[i];
            float mean = (float)sum / FFT_SIZE;
            for (int i = 0; i < FFT_SIZE; i++) {
                fft[2 * i] = (frame[i] - mean) / 32768.0f * window[i];
                fft[2 * i + 1] = 0;
            }
            dsps_fft2r_fc32(fft, FFT_SIZE);
            dsps_bit_rev_fc32(fft, FFT_SIZE);
            for (int b = 0; b < FFT_SIZE / 2; b++) binPower[b] += fft[2 * b] * fft[2 * b] + fft[2 * b + 1] * fft[2 * b + 1];
            for (int r = 0; r < height; r++) {
                float power = 0, counted = 0; // the row's loudest bin, and its loudest from firstBin up
                for (int b = binLow[r]; b <= binHigh[r] && b < FFT_SIZE / 2; b++) {
                    float binsPower = fft[2 * b] * fft[2 * b] + fft[2 * b + 1] * fft[2 * b + 1];
                    power = max(power, binsPower);
                    if (b >= firstBin) counted = max(counted, binsPower);
                }
                float value = 10 * log10f(power + 1e-12f);
                db[r * width + c] = value;
                if (binHigh[r] < firstBin || binLow[r] >= FFT_SIZE / 2) continue; // rumble only, or nothing: no part in the levels
                if (counted != power) value = 10 * log10f(counted + 1e-12f);      // the lowest row, without its rumble
                if (r >= levelFrom && r <= levelTo) {
                    loudnessCount[constrain((int)((value - LOUDNESS_FROM_DB) * 2), 0, LOUDNESS_STEPS - 1)]++;
                }
                if (value > loudest) {
                    loudest = value;
                    result.peakColumn = c;
                    result.peakRow = r;
                }
            }
            if (c % 16 == 0) vTaskDelay(1); // let the other tasks on this core run
        }
        if (outcome == DONE && superseded(number, grid, stamp)) outcome = SUPERSEDED;
        if (outcome == DONE) {
            // Where the moment's energy lies, whatever the scale of the grid:
            // bins from the lowest up, by share of the total
            const int lastBin = min(FFT_SIZE / 2 - 1, (int)(SPECTRUM_HIGH_HZ / binHz));
            double total = 0, below = 0;
            for (int b = 1; b <= lastBin; b++) total += binPower[b];
            result.bandLowHz = binHz;
            result.bandHighHz = lastBin * binHz;
            bool lowFound = false;
            for (int b = 1; b <= lastBin; b++) {
                below += binPower[b];
                if (!lowFound && below >= 0.1 * total) {
                    result.bandLowHz = b * binHz;
                    lowFound = true;
                }
                if (below >= 0.9 * total) {
                    result.bandHighHz = b * binHz;
                    break;
                }
            }
            result.momentNumber = number;
            result.width = width;
            result.height = height;
            result.highHz = highHz;
            result.db = db;
            result.loudestDb = loudest;
            // Down from the loudest step, until one cell in 200 is as loud as that or louder, and then half of them
            uint32_t cells = (uint32_t)width * levelRows, louder = 0;
            int step = LOUDNESS_STEPS;
            while (step > 0 && louder * 200 < cells) louder += loudnessCount[--step];
            result.brightDb = min(loudest, LOUDNESS_FROM_DB + step / 2.0f);
            while (step > 0 && louder * 2 < cells) louder += loudnessCount[--step];
            result.quietDb = min(loudest, LOUDNESS_FROM_DB + step / 2.0f);
            result.peakHz = spectrumRowHz(result.peakRow, height, highHz);
            result.seconds = (float)moment.length / rate;
            result.peakSeconds = (result.peakColumn + 0.5f) / width * result.seconds;
            int peakBin = firstBin;
            for (int b = 0; b < FFT_SIZE / 2; b++) {
                averageDb[b] = 10 * log10f(binPower[b] / width + 1e-12f);
                if (b >= firstBin && b * binHz <= highHz && binPower[b] > binPower[peakBin]) peakBin = b;
            }
            result.averageDb = averageDb;
            result.bins = FFT_SIZE / 2;
            result.binHz = binHz;
            result.averagePeakHz = peakBin * binHz;
            Serial.printf("[spectrum] Moment %d in %lu ms, 0 to %.0f kHz: peak %.0f Hz at %.2f s, 80%% of the energy %.0f-%.0f Hz; "
                          "loudest cell %.1f dB, one in 200 above %.1f, half above %.1f\n",
                          number, millis() - start, highHz / 1000, result.peakHz, result.peakSeconds, result.bandLowHz,
                          result.bandHighHz, result.loudestDb, result.brightDb, result.quietDb);
        }
    }
    heap_caps_free(fft);
    free(frame);
    free(window);
    free(binPower);
    free(binLow);
    free(binHigh);
    free(loudnessCount);
    return outcome;
}

// The result in hand is of the recording now asked about, for the grid now asked for
static bool ready(int number) { return readyNumber == number && readyGrid == wantedGrid && readyStamp == wantedStamp; }

static void spectrumTask(void *) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int number = wantedNumber;
        uint32_t grid = wantedGrid;
        if (number < 0 || ready(number)) continue;
        readyNumber = -1;
        // The screen may be half way through a frame drawn from the result: the pass waits for it
        xSemaphoreTake(resultLock, portMAX_DELAY);
        xSemaphoreGive(resultLock);
        uint32_t stamp = 0;
        Outcome outcome = compute(number, grid, stamp);
        if (outcome == DONE) {
            readyStamp = stamp;
            readyGrid = grid;
            readyNumber = number;
        } else if (outcome != SUPERSEDED) {
            // The screen says so. What is wanted stays as it is, since only
            // spectrumRequest() writes that: it takes the same request again as a new try.
            failedWhy = outcome;
            failedStamp = stamp;
            failedGrid = grid;
            failedNumber = number;
        }
    }
}

void spectrumInit() {
    if (!resultLock) resultLock = xSemaphoreCreateMutex();
}

void spectrumHold(bool hold) {
    if (!resultLock) return;
    if (hold) xSemaphoreTake(resultLock, portMAX_DELAY);
    else xSemaphoreGive(resultLock);
}

// The pass for what is now wanted ended without a picture
static bool failed(int number) {
    return failedNumber == number && wantedNumber == number && failedGrid == wantedGrid && failedStamp == wantedStamp;
}

void spectrumRequest(int momentNumber, int width, int height, int zoom) {
    uint32_t grid = gridWord(width, height, zoom);
    Moment moment;
    uint32_t stamp = findMoment(momentNumber, moment) ? momentStamp(moment) : 0;
    bool same = momentNumber == wantedNumber && grid == wantedGrid && stamp == wantedStamp;
    // Under way or done: the task has taken this very request up, or holds its
    // picture. (A pass given up because the list was half read at the time
    // has done neither: then this is the asking again that it waits for.)
    bool taken = passNumber == momentNumber && passGrid == grid && passStamp == stamp;
    if (same && !failed(momentNumber) && (taken || ready(momentNumber))) return;
    spectrumInit();
    if (resultLock && !task) xTaskCreatePinnedToCore(spectrumTask, "spectrum", 6144, NULL, 1, &task, 0);
    failedNumber = -1;
    wantedStamp = stamp;
    wantedGrid = grid;
    wantedNumber = momentNumber;
    if (!task) {
        Serial.println("[spectrum] no memory for the analysis task");
        failedWhy = FAILED;
        failedStamp = stamp;
        failedGrid = grid;
        failedNumber = momentNumber;
        return;
    }
    xTaskNotifyGive(task);
}

int spectrumProgress(int momentNumber) {
    if (ready(momentNumber)) return 100;
    // Only the figure of the pass asked for: not what the one before it had reached
    bool counting = wantedNumber == momentNumber && passNumber == momentNumber && passGrid == wantedGrid && passStamp == wantedStamp;
    return counting ? progress : 0;
}

const Spectrogram *spectrumResult(int momentNumber) {
    return ready(momentNumber) ? &result : nullptr;
}

bool spectrumFailed(int momentNumber) { return failed(momentNumber); }
bool spectrumUnreadable(int momentNumber) { return failed(momentNumber) && failedWhy == UNREADABLE; }
