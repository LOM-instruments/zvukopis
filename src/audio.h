#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>

// Sound moment engine on the board's ES8311 codec (mic in + speaker out over I2S).
// A task on core 0 streams audio and carries out the requests below.
// With an SD card in at startup, moments are saved to it and played back from it;
// otherwise they live in PSRAM and are lost on reset.

static const int SAMPLE_RATE = 48000;
static const int MAX_MOMENT_SECONDS = 60;
static const int MAX_MOMENTS = 500;
static const int WAVEFORM_BINS = 77;

// What a moment is about, chosen from the long-press menu
enum MomentTag : uint8_t {
    TAG_NONE,
    TAG_HUMAN,
    TAG_NATURE,
    TAG_FUNNY,
    TAG_ART,
    TAG_OTHER,
    TAG_COUNT,
};

struct Moment {
    int number;                      // shown as "Moment N"; numbers are never reused
    time_t recordedAt;               // UTC start of the recording, 0 if the clock wasn't set
    uint16_t startMs;                // milliseconds past recordedAt
    uint8_t tag;                     // MomentTag
    char fileName[32];               // on the SD card, in /moments
    int16_t *samples;                // mono, SAMPLE_RATE Hz; nullptr when the moment is on the SD card
    size_t length;                   // in samples
    uint32_t dataOffset;             // where the samples start in the moment's file on the SD card
    float gain;                      // playback boost so the loudest moment is near full scale
    uint8_t waveform[WAVEFORM_BINS]; // loudness overview for the list, 0-255
};

enum AudioState {
    AUDIO_IDLE,
    AUDIO_RECORDING,
    AUDIO_SAVING, // a finished recording is being written to the SD card
    AUDIO_PLAYING,
    AUDIO_SELF_TEST,
    AUDIO_FORMATTING, // the SD card is being formatted
};

// Requires Wire to be started on the PMU/codec I2C bus (boardPowerInit does that)
bool audioInit();

// Requests, carried out by the audio task within ~16 ms
void audioStartRecording();
void audioStopRecording();
void audioPlay(int index);
void audioStopPlayback();
void audioDelete(int index);
void audioSetTag(int index, MomentTag tag);
// Tape-style varispeed while playing: +1 speeds up for as long as it's held,
// -1 slows down, 0 drifts back to normal speed
void audioSpeedHold(int direction);
// Erases the SD card (FAT32) and drops its moments from the list; result in audioFormatResult()
void audioFormatCard();
int audioFormatResult(); // 0 none yet, 1 done, -1 failed
void audioSelfTest(); // speaker -> mic loopback check, results on serial
// The codec powers down by itself after 2 s idle and back up on the next
// record or play request.
// For sleep: suspend finishes any recording, stops playback, powers the codec
// down and returns once that's done.
void audioSuspend();
bool audioAsleep(); // codec off, I2S stopped, no requests pending

AudioState audioState();
float audioRecordingSeconds();
float audioRecordingLimitSeconds();
int audioPlayingIndex(); // -1 when not playing
float audioPlaybackSeconds();
float audioPlaybackSpeed(); // 1 = normal

// Mic gain in dB (0-54), or GAIN_AUTO; kept across restarts
static const int GAIN_AUTO = -1;
void audioSetMicGain(int db);
int audioMicGainSetting();
int audioMicGain(); // in use now (with auto gain, where it last settled)
float audioSecondsLeft(); // recording time that still fits on the SD card, or in memory
bool audioUsingSdCard();

// Moments are ordered newest first. momentListVersion() changes whenever the list does.
int momentCount();
bool momentGet(int index, Moment &out);
uint32_t momentListVersion();
