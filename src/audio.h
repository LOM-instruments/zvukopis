#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>

// Voice memo engine on the board's ES8311 codec (mic in + speaker out over I2S).
// A task on core 0 streams audio continuously and carries out the requests below.
// With an SD card in at startup, memos are saved to it and played back from it;
// otherwise they live in PSRAM and are lost on reset.

static const int SAMPLE_RATE = 16000;
static const int MAX_MEMO_SECONDS = 60;
static const int MAX_MEMOS = 500;
static const int WAVEFORM_BINS = 77;

struct Memo {
    int number;                      // shown as "Memo N"; numbers are never reused
    time_t recordedAt;               // UTC start of the recording, 0 if the clock wasn't set
    char fileName[32];               // on the SD card, in /memos
    int16_t *samples;                // mono, SAMPLE_RATE Hz; nullptr when the memo is on the SD card
    size_t length;                   // in samples
    uint32_t dataOffset;             // where the samples start in the memo's file on the SD card
    float gain;                      // playback boost so the loudest moment is near full scale
    uint8_t waveform[WAVEFORM_BINS]; // loudness overview for the list, 0-255
};

enum AudioState {
    AUDIO_IDLE,
    AUDIO_RECORDING,
    AUDIO_SAVING, // a finished recording is being written to the SD card
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
// For sleep: suspend finishes any recording, stops playback, powers the codec
// down and stops I2S, and returns once that's done. Resume undoes it.
void audioSuspend();
void audioResume();

AudioState audioState();
float audioRecordingSeconds();
float audioRecordingLimitSeconds();
int audioPlayingIndex(); // -1 when not playing
float audioPlaybackSeconds();
float audioSecondsLeft(); // recording time that still fits on the SD card, or in memory
bool audioUsingSdCard();

// Memos are ordered newest first. memoListVersion() changes whenever the list does.
int memoCount();
bool memoGet(int index, Memo &out);
uint32_t memoListVersion();
