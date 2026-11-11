#include "spectrum.h"

#include <Arduino.h>
#include <dsps_fft2r.h>
#include <math.h>

#include "audio.h"
#include "storage.h"

static const int FFT_SIZE = SAMPLE_RATE >= 96000 ? 4096 : 2048; // 23 Hz bins either way

static Spectrogram result;
static float *dbGrid = nullptr; // kept between analyses (PSRAM), grown when a request is bigger
static size_t dbGridCells = 0;
static float *averageDb = nullptr; // FFT_SIZE / 2
static volatile int readyNumber = -1;   // moment whose spectrogram is in `result`
static volatile int progress = 0;       // percent of the columns done
static volatile int wantedNumber = -1;  // moment being worked out next
static volatile int failedNumber = -1;  // moment whose analysis found no memory
// The grid size asked for, width in the upper half and height in the lower:
// one word, so that the task never takes the width of one request with the
// height of another. It changes when the button guide is shown or hidden.
static volatile uint32_t wantedSize = 0;
static TaskHandle_t task = nullptr;

static uint32_t gridSize(int width, int height) { return (uint32_t)width << 16 | (uint32_t)height; }

float spectrumRowHz(int row, int height) {
    return SPECTRUM_HIGH_HZ * (1 - (row + 0.5f) / height); // row 0 at the top
}

int spectrumHzRow(float hz, int height) {
    return constrain((int)((1 - hz / SPECTRUM_HIGH_HZ) * height), 0, height - 1);
}

// Reads `count` samples from `start` (zeros past the end)
class SampleSource {
  public:
    explicit SampleSource(const Moment &moment) : moment(moment) {
        if (!moment.samples) file = storageOpenMoment(moment);
    }
    ~SampleSource() {
        if (file) file.close();
    }
    void read(long start, int count, int16_t *out) {
        for (int i = 0; i < count; i++) out[i] = 0;
        long from = max(0L, start), to = min((long)moment.length, start + count);
        if (to <= from) return;
        if (moment.samples) {
            memcpy(out + (from - start), moment.samples + from, (to - from) * sizeof(int16_t));
        } else if (file) {
            file.seek(moment.dataOffset + from * sizeof(int16_t));
            file.read((uint8_t *)(out + (from - start)), (to - from) * sizeof(int16_t));
        }
    }

  private:
    const Moment &moment;
    File file;
};

enum Outcome { DONE, SUPERSEDED, FAILED };

// Another moment, or another grid size, has been asked for meanwhile
static bool superseded(int number, int width, int height) {
    return wantedNumber != number || wantedSize != gridSize(width, height);
}

static Outcome compute(int number, int width, int height) {
    int index = -1;
    Moment moment;
    for (int i = 0; momentGet(i, moment); i++) {
        if (moment.number == number) {
            index = i;
            break;
        }
    }
    progress = 0; // not the figure the analysis before this one ended on
    if (index < 0 || moment.length < FFT_SIZE / 4) return FAILED;
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
    double *rowEnergy = (double *)calloc(height, sizeof(double));
    double *binPower = (double *)ps_calloc(FFT_SIZE / 2, sizeof(double));
    // Bin range of each row: the bins inside its slice of the linear scale
    int *binLow = (int *)malloc(height * sizeof(int)), *binHigh = (int *)malloc(height * sizeof(int));
    bool ok = db && fft && frame && window && rowEnergy && binPower && binLow && binHigh;
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
        for (int r = 0; r < height; r++) {
            float upper = SPECTRUM_HIGH_HZ * (1 - (float)r / height);
            float lower = SPECTRUM_HIGH_HZ * (1 - (float)(r + 1) / height);
            binLow[r] = max(1, (int)ceilf(lower / binHz)); // bin 0 is DC
            binHigh[r] = max(binLow[r], (int)floorf(upper / binHz));
        }
        // Hann window
        for (int i = 0; i < FFT_SIZE; i++) window[i] = 0.5f - 0.5f * cosf(2 * (float)M_PI * i / (FFT_SIZE - 1));
        dsps_fft2r_init_fc32(NULL, FFT_SIZE);

        SampleSource source(moment);
        float loudest = -1000;
        for (int c = 0; c < width && !superseded(number, width, height); c++) {
            progress = c * 100 / width;
            long centre = (long)((c + 0.5) * moment.length / width);
            source.read(centre - FFT_SIZE / 2, FFT_SIZE, frame);
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
                float power = 0;
                for (int b = binLow[r]; b <= binHigh[r] && b < FFT_SIZE / 2; b++) {
                    power = max(power, fft[2 * b] * fft[2 * b] + fft[2 * b + 1] * fft[2 * b + 1]);
                }
                rowEnergy[r] += power;
                float value = 10 * log10f(power + 1e-12f);
                db[r * width + c] = value;
                if (value > loudest && spectrumRowHz(r, height) >= PEAK_MIN_HZ) {
                    loudest = value;
                    result.peakColumn = c;
                    result.peakRow = r;
                }
            }
            if (c % 16 == 0) vTaskDelay(1); // let the other tasks on this core run
        }
        if (superseded(number, width, height)) outcome = SUPERSEDED;
        if (outcome == DONE) {
            // Where the energy lies: rows from the bottom (low) up, by share of the total
            double total = 0, below = 0;
            for (int r = 0; r < height; r++) total += rowEnergy[r];
            result.bandLowHz = 0;
            result.bandHighHz = SPECTRUM_HIGH_HZ;
            bool lowFound = false;
            for (int r = height - 1; r >= 0; r--) {
                below += rowEnergy[r];
                if (!lowFound && below >= 0.1 * total) {
                    result.bandLowHz = spectrumRowHz(r, height);
                    lowFound = true;
                }
                if (below >= 0.9 * total) {
                    result.bandHighHz = spectrumRowHz(r, height);
                    break;
                }
            }
            result.momentNumber = number;
            result.width = width;
            result.height = height;
            result.db = db;
            result.loudestDb = loudest;
            result.peakHz = spectrumRowHz(result.peakRow, height);
            result.seconds = (float)moment.length / rate;
            result.peakSeconds = (result.peakColumn + 0.5f) / width * result.seconds;
            int firstBin = (int)ceilf(PEAK_MIN_HZ / binHz), peakBin = firstBin;
            for (int b = 0; b < FFT_SIZE / 2; b++) {
                averageDb[b] = 10 * log10f(binPower[b] / width + 1e-12f);
                if (b >= firstBin && b * binHz <= SPECTRUM_HIGH_HZ && binPower[b] > binPower[peakBin]) peakBin = b;
            }
            result.averageDb = averageDb;
            result.bins = FFT_SIZE / 2;
            result.binHz = binHz;
            result.averagePeakHz = peakBin * binHz;
            Serial.printf("[spectrum] Moment %d in %lu ms: peak %.0f Hz at %.2f s, 80%% of the energy %.0f-%.0f Hz\n",
                          number, millis() - start, result.peakHz, result.peakSeconds, result.bandLowHz,
                          result.bandHighHz);
        }
    }
    heap_caps_free(fft);
    free(frame);
    free(window);
    free(rowEnergy);
    free(binPower);
    free(binLow);
    free(binHigh);
    return outcome;
}

// The result in hand is that moment's, at the grid size now asked for
static bool ready(int number) {
    return readyNumber == number && gridSize(result.width, result.height) == wantedSize;
}

static void spectrumTask(void *) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int number = wantedNumber;
        uint32_t size = wantedSize;
        if (number < 0 || ready(number)) continue;
        readyNumber = -1;
        Outcome outcome = compute(number, size >> 16, size & 0xFFFF);
        if (outcome == DONE) {
            readyNumber = number;
        } else if (outcome == FAILED && !superseded(number, size >> 16, size & 0xFFFF)) {
            // The screen says so, and the request is forgotten, so that asking again tries again
            failedNumber = number;
            wantedNumber = -1;
        }
    }
}

void spectrumRequest(int momentNumber, int width, int height) {
    uint32_t size = gridSize(width, height);
    if (momentNumber == wantedNumber && size == wantedSize) return;
    if (!task) xTaskCreatePinnedToCore(spectrumTask, "spectrum", 6144, NULL, 1, &task, 0);
    if (!task) {
        Serial.println("[spectrum] no memory for the analysis task");
        failedNumber = momentNumber;
        return;
    }
    failedNumber = -1;
    wantedSize = size;
    wantedNumber = momentNumber;
    xTaskNotifyGive(task);
}

int spectrumProgress(int momentNumber) {
    if (ready(momentNumber)) return 100;
    return wantedNumber == momentNumber ? progress : 0;
}

const Spectrogram *spectrumResult(int momentNumber) {
    return ready(momentNumber) ? &result : nullptr;
}

bool spectrumFailed(int momentNumber) { return failedNumber == momentNumber; }
