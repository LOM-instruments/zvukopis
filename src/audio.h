#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>

// Sound moment engine on the board's ES8311 codec (mic in + speaker out over I2S).
// A task on core 0 streams audio and carries out the requests below.
// With an SD card in at startup, moments are saved to it and played back from it;
// otherwise they live in PSRAM and are lost on reset.

// 96 kHz: the microphone has real output to 40 kHz (measured). Moments made
// at 48 kHz (older firmware, or a build with -DSAMPLE_RATE_HZ=48000) still
// load and play; each moment carries its own rate.
#ifndef SAMPLE_RATE_HZ
#define SAMPLE_RATE_HZ 96000
#endif
static const int SAMPLE_RATE = SAMPLE_RATE_HZ;
// The lowest useful mic gain: below it the codec's own floor (about -92 dBFS)
// swallows the signal, and nothing is loud enough to need the headroom
static const int MIN_GAIN_DB = 18;
static const uint32_t MAX_RECORDING_SECONDS = (SAMPLE_RATE >= 96000 ? 6 : 12) * 3600; // one WAV file can't pass 4 GB
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
    char folder[12];                 // on the SD card: the day's folder in /moments ("2026-10-28", or "undated")
    char fileName[48];               // ... and the file in it: 2026-10-28T10-05-15_nature.wav
    int16_t *samples;                // mono, SAMPLE_RATE Hz; nullptr when the moment is on the SD card
    size_t length;                   // in samples
    uint32_t sampleRate;             // of this moment's samples (older files: 48000)
    uint32_t dataOffset;             // where the samples start in the moment's file on the SD card
    float gain;                      // playback boost so the loudest moment is near full scale
    uint8_t waveform[WAVEFORM_BINS]; // loudness overview for the list, 0-255
    uint8_t micGainDb;               // the codec's gain when the recording ended (for the file's bext description)
    uint8_t micGainAuto;             // ... and whether that was auto gain
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
void audioStartRecording(uint32_t seconds = MAX_RECORDING_SECONDS); // until stopped, or what fits
void audioStopRecording();
void audioCancelRecording(); // stops and discards it
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
float audioLongestRecording(); // seconds one recording can last: the card's room, or memory's without a card
bool audioUsingSdCard();

// Moments are ordered newest first. momentListVersion() changes whenever the list does.
int momentCount();
bool momentGet(int index, Moment &out);
uint32_t momentListVersion();
