#pragma once

#include <stdint.h>

#include "audio.h"

// Frequency analysis of one moment, worked out in a background task since it
// takes a couple of seconds: a spectrogram (loudness over a linear frequency
// by time grid; rows top = highest) and the moment's average spectrum.
// 4096-point FFTs with a Hann window (2048 points in a 48 kHz build).

static const float SPECTRUM_HIGH_HZ = SAMPLE_RATE / 2.0f; // the widest scale of both views runs from 0 Hz to this (48 kHz)
static const float PEAK_MIN_HZ = 40; // the loudest point, and the level everything is shown against, from here up
static const int SPECTRUM_MAX_ZOOM = 255; // see spectrumRequest
// brightDb and quietDb are taken from the lowest quarter of the range (up to
// 12 kHz), whatever the scale asked for: the part every scale of the screen
// shows, and where most sound is. So the ink of a picture hardly changes
// with its scale, and the quiet rows above the hearing range do not pass for
// the background of what is heard.
static const int SPECTRUM_LEVELS_ZOOM = 4;
// A cell at or under this holds no sound at all (the 1e-12 added before the
// logarithm is -120 dB): the samples were all zero, or the row lies past
// what the recording's sample rate reaches
static const float SPECTRUM_SILENT_DB = -119;

struct Spectrogram {
    int momentNumber;
    int width, height;
    float highHz;    // the rows run from 0 Hz to this: SPECTRUM_HIGH_HZ, or the part of it asked for
    const float *db; // width * height, row by row: loudness in dB (any scale); the screen picks the contrast
    float loudestDb; // the loudest cell from PEAK_MIN_HZ up, on this frequency scale (like the peak below)
    // The loudness only one cell in 200 passes, of those between PEAK_MIN_HZ
    // and SPECTRUM_HIGH_HZ / SPECTRUM_LEVELS_ZOOM. A whistle puts a few cells
    // far above everything else: this says where the bulk of the sound ends,
    // for the screen to set its ink by
    float brightDb;
    // The loudness half of those cells are under: the background, unless
    // sound fills most of them
    float quietDb;
    int peakColumn, peakRow;
    float peakHz, peakSeconds;
    float bandLowHz, bandHighHz; // 10% and 90% of the moment's energy lie below these, whatever the scale
    float seconds;               // the moment's length
    // Average spectrum over the whole moment, in dB, one value per FFT bin
    // (all of them, whatever the scale)
    const float *averageDb;
    int bins;
    float binHz;
    float averagePeakHz; // its loudest bin on this frequency scale, from PEAK_MIN_HZ up
};

// Starts working out the spectrogram of moment `number` at this grid size
// (unless it's already there or under way), on a frequency scale from 0 Hz
// to SPECTRUM_HIGH_HZ / zoom: 1 for all of it, 2 for the lower half, 4 for
// the lowest quarter (up to SPECTRUM_MAX_ZOOM). The same moment at another
// size (the button guide shown or hidden) or zoom is worked out again, and
// so is another recording that has come to carry the number (another card):
// ask again whenever the list of moments has changed.
void spectrumRequest(int momentNumber, int width, int height, int zoom);
// The finished spectrogram of that moment at the size and zoom last asked
// for, or nullptr while it's being worked out
const Spectrogram *spectrumResult(int momentNumber);
int spectrumProgress(int momentNumber); // 0-100 while it's being worked out
// The analysis of that moment found no memory to work in, or could not read
// the recording (spectrumUnreadable says which). Asking for it again tries
// again.
bool spectrumFailed(int momentNumber);
bool spectrumUnreadable(int momentNumber);
// Once, before anything else here is used
void spectrumInit();
// Around drawing from spectrumResult(): true before the result is asked for,
// false when the frame is done. A new analysis waits for the frame instead
// of writing into the grid under it.
void spectrumHold(bool hold);

// Frequency at the middle of a row (linear) of a grid that runs up to `highHz`
float spectrumRowHz(int row, int height, float highHz);
int spectrumHzRow(float hz, int height, float highHz);
