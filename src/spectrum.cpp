#include "spectrum.h"

#include <Arduino.h>
#include <dsps_fft2r.h>
#include <math.h>

#include "audio.h"
#include "storage.h"

static const int FFT_SIZE = 2048; // 23 Hz bins at 48 kHz
static const float PEAK_MIN_HZ = 40; // the loudest point is looked for above this: below is DC offset and rumble

static Spectrogram result;
static uint8_t *cells = nullptr;
static float *averageDb = nullptr; // FFT_SIZE / 2
static volatile int readyNumber = -1;   // moment whose spectrogram is in `result`
static volatile int wantedNumber = -1;  // moment being worked out next
static int wantedWidth = 0, wantedHeight = 0;
static TaskHandle_t task = nullptr;

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

static bool compute(int number, int width, int height) {
    int index = -1;
    Moment moment;
    for (int i = 0; momentGet(i, moment); i++) {
        if (moment.number == number) {
            index = i;
            break;
        }
    }
    if (index < 0 || moment.length < FFT_SIZE / 4) return false;
    uint32_t start = millis();

    float *db = (float *)ps_malloc(width * height * sizeof(float));
    float *fft = (float *)heap_caps_aligned_alloc(16, FFT_SIZE * 2 * sizeof(float), MALLOC_CAP_INTERNAL);
    int16_t *frame = (int16_t *)malloc(FFT_SIZE * sizeof(int16_t));
    float *window = (float *)malloc(FFT_SIZE * sizeof(float));
    double *rowEnergy = (double *)calloc(height, sizeof(double));
    double *binPower = (double *)ps_calloc(FFT_SIZE / 2, sizeof(double));
    // Bin range of each row: the bins inside its slice of the linear scale
    int *binLow = (int *)malloc(height * sizeof(int)), *binHigh = (int *)malloc(height * sizeof(int));
    bool ok = db && fft && frame && window && rowEnergy && binPower && binLow && binHigh;
    if (ok && !cells) cells = (uint8_t *)ps_malloc(width * height);
    if (ok && !averageDb) averageDb = (float *)ps_malloc(FFT_SIZE / 2 * sizeof(float));
    ok = ok && cells && averageDb && width * height <= wantedWidth * wantedHeight;
    if (ok) {
        const float binHz = (float)SAMPLE_RATE / FFT_SIZE;
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
        for (int c = 0; c < width && wantedNumber == number; c++) {
            long centre = (long)((c + 0.5) * moment.length / width);
            source.read(centre - FFT_SIZE / 2, FFT_SIZE, frame);
            for (int i = 0; i < FFT_SIZE; i++) {
                fft[2 * i] = frame[i] / 32768.0f * window[i];
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
        ok = wantedNumber == number;
        if (ok) {
            for (int i = 0; i < width * height; i++) {
                float level = constrain((db[i] - (loudest - SPECTRUM_RANGE_DB)) / SPECTRUM_RANGE_DB, 0.0f, 1.0f);
                cells[i] = (uint8_t)(level * level * 255); // squared, so the noise floor stays light
            }
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
            result.cells = cells;
            result.peakHz = spectrumRowHz(result.peakRow, height);
            result.seconds = (float)moment.length / SAMPLE_RATE;
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
    free(db);
    heap_caps_free(fft);
    free(frame);
    free(window);
    free(rowEnergy);
    free(binPower);
    free(binLow);
    free(binHigh);
    return ok;
}

static void spectrumTask(void *) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int number = wantedNumber;
        if (number < 0 || number == readyNumber) continue;
        readyNumber = -1;
        if (compute(number, wantedWidth, wantedHeight)) readyNumber = number;
    }
}

void spectrumRequest(int momentNumber, int width, int height) {
    if (momentNumber == wantedNumber) return;
    if (!task) {
        // The grid size is fixed by the screen, so the buffer is sized on first use
        wantedWidth = width;
        wantedHeight = height;
        xTaskCreatePinnedToCore(spectrumTask, "spectrum", 6144, NULL, 1, &task, 0);
    }
    wantedNumber = momentNumber;
    xTaskNotifyGive(task);
}

const Spectrogram *spectrumResult(int momentNumber) {
    return readyNumber == momentNumber ? &result : nullptr;
}
