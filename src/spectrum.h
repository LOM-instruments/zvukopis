#pragma once

#include <stdint.h>

// Spectrogram of one moment, for the frequency analysis screen: loudness over
// a log-frequency (rows, top = highest) by linear-time (columns) grid, worked
// out in a background task since it takes a couple of seconds.

static const float SPECTRUM_LOW_HZ = 40;
static const float SPECTRUM_HIGH_HZ = 20000;
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
};

// Starts working out the spectrogram of moment `number` at this grid size
// (unless it's already there or under way)
void spectrumRequest(int momentNumber, int width, int height);
// The finished spectrogram of that moment, or nullptr while it's being worked out
const Spectrogram *spectrumResult(int momentNumber);

// Frequency at the middle of a row
float spectrumRowHz(int row, int height);
int spectrumHzRow(float hz, int height);
