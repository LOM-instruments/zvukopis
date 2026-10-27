#pragma once

#include <stdint.h>

// Frequency analysis of one moment, worked out in a background task since it
// takes a couple of seconds: a spectrogram (loudness over a linear frequency
// by time grid; rows top = highest) and the moment's average spectrum.
// 2048-point FFTs with a Hann window.

static const float SPECTRUM_HIGH_HZ = 20000; // both views run from 0 Hz to this
static const float SPECTRUM_RANGE_DB = 60; // cells span this far below the loudest point

struct Spectrogram {
    int momentNumber;
    int width, height;
    const uint8_t *cells; // width * height, row by row: 0 = RANGE_DB below the peak or quieter, 255 = the
                          // peak, on a squared scale so the noise floor stays light
    int peakColumn, peakRow;
    float peakHz, peakSeconds;
    float bandLowHz, bandHighHz; // 10% and 90% of the energy lie below these
    float seconds;               // the moment's length
    // Average spectrum over the whole moment, in dB, one value per FFT bin
    const float *averageDb;
    int bins;
    float binHz;
    float averagePeakHz;
};

// Starts working out the spectrogram of moment `number` at this grid size
// (unless it's already there or under way)
void spectrumRequest(int momentNumber, int width, int height);
// The finished spectrogram of that moment, or nullptr while it's being worked out
const Spectrogram *spectrumResult(int momentNumber);

// Frequency at the middle of a row (linear)
float spectrumRowHz(int row, int height);
int spectrumHzRow(float hz, int height);
