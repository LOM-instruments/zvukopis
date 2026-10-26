#pragma once

#include <stddef.h>
#include <stdint.h>

// Voice memo engine on the board's ES8311 codec (mic in + speaker out over I2S).
// A task on core 0 streams audio continuously and carries out the requests below.
// Memos live in PSRAM, so they are lost on reset.

static const int SAMPLE_RATE = 16000;
static const int MAX_MEMO_SECONDS = 60;
static const int MAX_MEMOS = 50;
static const int WAVEFORM_BINS = 77;

struct Memo {
    int number;                      // shown as "Memo N"; numbers are never reused
    int16_t *samples;                // mono, SAMPLE_RATE Hz
    size_t length;                   // in samples
    float gain;                      // playback boost so the loudest moment is near full scale
    uint8_t waveform[WAVEFORM_BINS]; // loudness overview for the list, 0-255
};

enum AudioState {
    AUDIO_IDLE,
    AUDIO_RECORDING,
    AUDIO_PLAYING,
    AUDIO_SELF_TEST,
};

// Requires Wire to be started on the PMU/codec I2C bus (boardPowerInit does that)
bool audioInit();

// Requests, carried out by the audio task within ~16 ms
void audioStartRecording();
void audioStopRecording();
void audioPlay(int index);
void audioStopPlayback();
void audioDelete(int index);
void audioSelfTest(); // speaker -> mic loopback check, results on serial

AudioState audioState();
float audioRecordingSeconds();
float audioRecordingLimitSeconds();
int audioPlayingIndex(); // -1 when not playing
float audioPlaybackSeconds();
float audioSecondsLeft(); // recording time that still fits in memory

// Memos are ordered newest first. memoListVersion() changes whenever the list does.
int memoCount();
bool memoGet(int index, Memo &out);
uint32_t memoListVersion();
