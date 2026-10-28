#pragma once

#include <FS.h>
#include <sdmmc_cmd.h>

#include "audio.h"

// Moment files on the microSD card (SDMMC, 4-bit): one WAV per moment in a
// folder per day, /moments/2026-10-26/2026-10-26T14-05-33_nature.wav (ISO 8601
// date and time, dashes in the time since FAT names can't hold colons, then
// the tag), or /moments/undated/MOMENT_0001.wav while the clock isn't set.
// Re-tagging a moment renames its file. 48 kHz mono 16-bit, so they play
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
bool storageUpdateMoment(Moment &moment); // writes its tag to the file and renames it to match
// The file name a moment gets (or would get) from its time and tag
void storageMomentFileName(const Moment &moment, char *buf, size_t len);
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
