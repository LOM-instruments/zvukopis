#pragma once

#include <FS.h>
#include <sdmmc_cmd.h>

#include "audio.h"

// Moment files on the microSD card (SDMMC, 4-bit), one WAV per moment in /moments,
// named after the local time the recording started (2026-10-26_14-05-33.wav),
// or MOMENT_0001.wav while the clock isn't set. 48 kHz mono 16-bit, so they play
// on a computer too. A Broadcast Wave "bext" chunk carries the time-of-day
// timecode, and a custom "zvkp" chunk the moment number, start time, tag,
// playback gain and list waveform, so the list can be built at boot from the
// file headers alone.
// Only used by the audio task after storageInit().

// RIFF + fmt + bext + zvkp + data chunk headers; the samples follow
static const size_t WAV_HEADER_BYTES = 12 + (8 + 16) + (8 + 602) + (8 + 100) + 8;

bool storageInit(); // mounts the card; false when there is none or it can't be read
bool storageAvailable();

// A card that answers but can't be mounted is most likely exFAT (fresh cards
// over 32 GB): this firmware reads FAT only, so it has to be formatted first
enum CardState { SD_ABSENT, SD_UNREADABLE, SD_MOUNTED };
CardState storageCardState();

// Appends the moments found on the card to `out` (up to `max`), in any order.
// Their samples stay on the card: samples is nullptr and dataOffset is set.
int storageLoadMoments(Moment *out, int max);

bool storageSaveMoment(Moment &moment, const int16_t *samples); // fills in fileName and dataOffset
bool storageDeleteMoment(const Moment &moment);
bool storageUpdateMoment(const Moment &moment); // writes its tag to the file
File storageOpenMoment(const Moment &moment);   // positioned at the first sample

// The WAV header a moment's file starts with (WAV_HEADER_BYTES); for sending
// moments held in memory as files
size_t storageWavHeader(const Moment &moment, uint8_t *buf);

uint64_t storageFreeBytes(); // estimate, kept current as moments are saved and deleted
uint64_t storageTotalBytes();

// Erases the card with a fresh FAT file system and mounts it again
bool storageFormat();
// Unmounts the file system and returns the card for raw sector access (USB card reader)
sdmmc_card_t *storageOpenRawCard();
