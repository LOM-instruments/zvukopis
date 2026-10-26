#pragma once

#include <FS.h>

#include "audio.h"

// Memo files on the microSD card (SDMMC, 4-bit), one WAV per memo in /memos:
// MEMO_0001.WAV, ... 16 kHz mono 16-bit, so they play on a computer too.
// A custom "memo" chunk carries the playback gain and list waveform, so the
// list can be built at boot from the file headers alone.
// Only used by the audio task after storageInit().

bool storageInit(); // mounts the card; false when there is none
bool storageAvailable();

// Appends the memos found on the card to `out` (up to `max`), in any order.
// Their samples stay on the card: samples is nullptr and dataOffset is set.
int storageLoadMemos(Memo *out, int max);

bool storageSaveMemo(const Memo &memo, const int16_t *samples);
bool storageDeleteMemo(int number);
File storageOpenMemo(const Memo &memo); // positioned at the first sample

uint64_t storageFreeBytes(); // estimate, kept current as memos are saved and deleted
