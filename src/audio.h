#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "config.h"

// Sound moment engine on the board's ES8311 codec (mic in + speaker out over I2S).
// A task on core 0 streams audio and carries out the requests below.
// With an SD card in, moments are saved to it and played back from it;
// otherwise they live in PSRAM and are lost on reset. The card is looked for
// at startup, and again whenever the codec wakes for a recording or playback:
// one taken out or put in since is noticed then, and the list follows.

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

// What a moment is about, chosen from the long-press menu: the slot of a
// category (config.h), 1..CATEGORY_MAX, or TAG_NONE
static const int TAG_NONE = 0;

struct Moment {
    int number;                      // shown as "Moment N"; numbers are never reused
    time_t recordedAt;               // UTC start of the recording, 0 if the clock wasn't set
    uint16_t startMs;                // milliseconds past recordedAt
    uint8_t tag;                     // category slot, TAG_NONE for none
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
    AUDIO_USB_MIC,    // the mic streams to the computer over USB (usbmic.h), until a restart
};

// Requires Wire to be started on the PMU/codec I2C bus (boardPowerInit does that)
bool audioInit();

// Requests, carried out by the audio task within ~16 ms.
// Play, delete and tag take a place in the list as the caller sees it now.
// The request carries that moment's number, and the audio task finds the
// moment by it, wherever the list has moved it to meanwhile. Delete and tag
// are not carried out while a recording runs: the card work would cost it sound.
void audioStartRecording(uint32_t seconds = MAX_RECORDING_SECONDS); // until stopped, or what fits
void audioStopRecording();
void audioCancelRecording(); // stops and discards it
void audioPlay(int index);
void audioStopPlayback();
void audioDelete(int index);
// Deletes the moment with this number, wherever it sits in the list by the
// time the request is carried out (the list shifts when a recording is saved)
void audioDeleteNumber(int number);
// Has the audio task look at the card slot: a card put in since is mounted
// and its moments listed, one taken out leaves the list. Nothing while
// something records or plays; those look for themselves when they start.
void audioRecheckCard();
void audioSetTag(int index, int tag); // a category slot, or TAG_NONE
void audioClearTagsAbove(int count); // moments tagged with a slot above `count` lose the tag
// Tape-style varispeed while playing: +1 speeds up for as long as it's held,
// -1 slows down, 0 lets go: the speed drifts back to normal, or stays, by the
// setting; a tap then brings it back
void audioSpeedHold(int direction);
void audioSpeedTap();
// Erases the SD card (FAT32) and drops its moments from the list; result in audioFormatResult()
void audioFormatCard();
int audioFormatResult(); // 0 none yet, 1 done, -1 failed
void audioSelfTest(); // speaker -> mic loopback check, results on serial
void audioStartUsbMic(); // the mic's samples go to usbMicFeed() from now on; gain changes still apply
// The codec powers down by itself after 2 s idle and back up on the next
// record or play request.
// For sleep: suspend finishes any recording, stops playback, powers the codec
// down and returns once that's done.
void audioSuspend();
bool audioAsleep(); // codec off, I2S stopped, no requests pending

AudioState audioState();
// False when the audio chain did not start (codec or I2S): nothing can be
// recorded or played, and requests are dropped
bool audioReady();
// The list holds MAX_MOMENTS: no room for another, whatever the card has free
bool audioListFull();
// Something that went wrong with a recording, for the screen and the web page
// to say. It stays until the next recording starts.
enum AudioNotice {
    AUDIO_NOTICE_NONE,
    AUDIO_NOTICE_CARD_FAILED, // the card stopped taking data: the recording ended there, what was written is kept
    AUDIO_NOTICE_NOT_SAVED,   // a recording could not be kept at all
    AUDIO_NOTICE_NO_CARD,     // the card that was there is gone (until one is mounted again, or the next recording starts)
};
AudioNotice audioNotice(uint32_t &since); // and when it happened (millis)
float audioRecordingSeconds();
float audioRecordingLimitSeconds();
int audioPlayingIndex(); // -1 when not playing
float audioPlaybackSeconds();
float audioPlaybackSpeed(); // 1 = normal

// Mic gain in dB (0-54), or GAIN_AUTO; kept across restarts
static const int GAIN_AUTO = -1;
void audioSetMicGain(int db);
int audioMicGainSetting();

// Varispeed (holding up or down while a moment plays): the top speed, in
// times normal (2, 3, 4 or 6), and how quickly the speed moves
enum VarispeedRamp { RAMP_GENTLE, RAMP_NORMAL, RAMP_QUICK, RAMP_COUNT };
static const int VARISPEED_MAX_CHOICES[] = {2, 3, 4, 6};
static const int VARISPEED_MAX_CHOICE_COUNT = 4;
void audioSetVarispeed(int maxSpeed, int ramp, int hold); // kept in settings; -1 leaves that one as it is
int audioVarispeedMax();
int audioVarispeedRamp();
bool audioVarispeedHold(); // letting go keeps the speed (true) or glides back to normal
const char *varispeedRampName(int ramp); // "gentle", "normal", "quick"
int audioMicGain(); // in use now (with auto gain, where it last settled)
float audioSecondsLeft(); // recording time that still fits on the SD card, or in memory
float audioLongestRecording(); // seconds one recording can last: the card's room, or memory's without a card
bool audioUsingSdCard();

// Moments are ordered newest first. momentListVersion() changes whenever the list does.
int momentCount();
bool momentGet(int index, Moment &out);
uint32_t momentListVersion();
