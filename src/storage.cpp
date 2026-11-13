#include "storage.h"

#include "config.h"

#include <Arduino.h>
#include <SD_MMC.h>
#include <driver/sdmmc_host.h>
#include <sdmmc_cmd.h>
#include <diskio_impl.h>
#include <diskio_sdmmc.h>
#include <ff.h>

// Same wiring as Waveshare's SD examples
static const int SD_CLK_PIN = 16;
static const int SD_CMD_PIN = 17;
static const int SD_D0_PIN = 15;
static const int SD_D1_PIN = 7;
static const int SD_D2_PIN = 8;
static const int SD_D3_PIN = 18;

static const char *MOUNT_POINT = "/sdcard";
static const char *MOMENT_DIR = "/moments";
static const uint32_t MOMENT_CHUNK_VERSION = 2;

static bool mounted = false;
static bool cardLent = false; // handed out for raw access (the USB card reader), until the restart that ends it
static CardState cardState = SD_ABSENT;
static volatile uint32_t mounts = 0; // mounted or let go so many times (storageMounts)
static uint64_t freeBytes = 0, totalBytes = 0;

// "zvkp" chunk payload: what the list needs, so it can be built from headers alone
struct __attribute__((packed)) MomentChunk {
    uint32_t version;
    int32_t number;
    int64_t recordedAt; // UTC, 0 if unknown
    float gain;
    uint8_t waveform[WAVEFORM_BINS];
    uint8_t tag;      // since version 2
    uint16_t startMs; // since version 2
};

struct __attribute__((packed)) WavFormat {
    uint16_t audioFormat; // 1 = PCM
    uint16_t channels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
};

// Broadcast Wave "bext" chunk (EBU Tech 3285). Its TimeReference is the
// time-of-day timecode DAWs use to place a recording on their timeline.
struct __attribute__((packed)) BextChunk {
    char description[256];
    char originator[32];
    char originatorReference[32];
    char originationDate[10]; // yyyy-mm-dd
    char originationTime[8];  // hh:mm:ss
    uint32_t timeReferenceLow; // samples since local midnight
    uint32_t timeReferenceHigh;
    uint16_t version;
    uint8_t umid[64];
    int16_t loudness[5];
    uint8_t reserved[180];
};

static const uint32_t EXTRA_PADDED = sizeof(MomentChunk) + (sizeof(MomentChunk) & 1); // RIFF chunks are word aligned
static_assert(sizeof(BextChunk) == 602, "bext is 602 bytes before the coding history");
static_assert(WAV_HEADER_BYTES == 12 + (8 + sizeof(WavFormat)) + (8 + sizeof(BextChunk)) + (8 + EXTRA_PADDED) + 8,
              "WAV_HEADER_BYTES is out of date");
static_assert(WAV_HEADER_BYTES % sizeof(int16_t) == 0, "the writer counts the header in samples");

static sdmmc_card_t *openRawCard(bool quiet = false);
static bool streamReady();
static void renameToMatch(Moment &moment);

static void folderPath(char *buf, size_t len, const Moment &moment) {
    if (moment.folder[0]) snprintf(buf, len, "%s/%s", MOMENT_DIR, moment.folder);
    else snprintf(buf, len, "%s", MOMENT_DIR); // files from before the day folders
}

static void momentPath(char *buf, size_t len, const Moment &moment) {
    char folder[32];
    folderPath(folder, sizeof(folder), moment);
    snprintf(buf, len, "%s/%s", folder, moment.fileName);
}

// "_nature": the category's name, lower case, anything but letters and digits as "-"
static void tagSuffix(const Moment &moment, char *buf, size_t len) {
    buf[0] = 0;
    if (moment.tag == TAG_NONE || len < 3) return;
    Category c = configCategory(moment.tag);
    size_t n = 0;
    buf[n++] = '_';
    bool dash = false;
    for (const char *p = c.name; *p && n < len - 1; p++) {
        if (isalnum((uint8_t)*p)) {
            buf[n++] = tolower((uint8_t)*p);
            dash = false;
        } else if (!dash) {
            buf[n++] = '-';
            dash = true;
        }
    }
    if (n > 1 && buf[n - 1] == '-') n--;
    buf[n] = 0;
    if (n == 1) buf[0] = 0; // a name with nothing usable in it
}

void storageMomentFileName(const Moment &moment, char *buf, size_t len) {
    char base[24], suffix[CATEGORY_NAME_LEN + 2];
    if (moment.recordedAt) {
        tm local;
        localtime_r(&moment.recordedAt, &local);
        strftime(base, sizeof(base), "%Y-%m-%dT%H-%M-%S", &local);
    } else {
        snprintf(base, sizeof(base), "MOMENT_%04d", moment.number);
    }
    tagSuffix(moment, suffix, sizeof(suffix));
    snprintf(buf, len, "%s%s.wav", base, suffix);
}

// The day's folder and a file name from the time and tag, made unique if taken
static void chooseFileName(Moment &moment) {
    if (moment.recordedAt) {
        tm local;
        localtime_r(&moment.recordedAt, &local);
        strftime(moment.folder, sizeof(moment.folder), "%Y-%m-%d", &local);
    } else {
        strlcpy(moment.folder, "undated", sizeof(moment.folder));
    }
    char folder[32], path[96];
    folderPath(folder, sizeof(folder), moment);
    if (!SD_MMC.exists(folder)) SD_MMC.mkdir(folder);
    storageMomentFileName(moment, moment.fileName, sizeof(moment.fileName));
    momentPath(path, sizeof(path), moment);
    if (SD_MMC.exists(path)) {
        char *dot = strrchr(moment.fileName, '.');
        if (dot) snprintf(dot, sizeof(moment.fileName) - (dot - moment.fileName), "_%d.wav", moment.number);
    }
}

static void updateSpace() {
    totalBytes = SD_MMC.totalBytes();
    freeBytes = totalBytes - SD_MMC.usedBytes();
}

static bool mountCard() {
    SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN, SD_D1_PIN, SD_D2_PIN, SD_D3_PIN);
    mounted = SD_MMC.begin(MOUNT_POINT, false);
    if (!mounted) return false;
    mounts = mounts + 1;
    cardState = SD_MOUNTED;
    if (!SD_MMC.exists(MOMENT_DIR)) SD_MMC.mkdir(MOMENT_DIR);
    updateSpace();
    Serial.printf("SD card mounted: %llu MB, %llu MB free\n", SD_MMC.cardSize() >> 20, freeBytes >> 20);
    // What recording onto the card needs is made now, not when the first
    // recording starts: later the web interface may be up, and the radio
    // lives on the same internal RAM as the write buffer
    if (!streamReady()) Serial.println("[storage] no memory for recording onto the SD card");
    return true;
}

// A card that answers but didn't mount
static void cardUnreadable(const sdmmc_card_t *card) {
    cardState = SD_UNREADABLE;
    Serial.printf("SD card found (%llu MB) but its file system can't be read (exFAT?): format it in Settings\n",
                  (uint64_t)card->csd.capacity * card->csd.sector_size >> 20);
}

bool storageInit() {
    if (mountCard()) return true;
    sdmmc_card_t *card = openRawCard();
    if (card) {
        cardUnreadable(card);
        sdmmc_host_deinit();
    } else {
        cardState = SD_ABSENT;
        Serial.println("No SD card, moments are kept in memory only");
    }
    return false;
}

bool storageAvailable() { return mounted; }

bool storageRecheck() {
    if (cardLent) return false;
    if (mounted) {
        // The folder has been there since the mount, so a no can only mean that the card doesn't answer
        if (SD_MMC.exists(MOMENT_DIR)) return false;
        Serial.println("[storage] the SD card has stopped answering: starting it again");
        SD_MMC.end();
        mounted = false;
        mounts = mounts + 1;
        cardState = SD_ABSENT;
        storageInit();
        return true;
    }
    // An empty slot fails this first step quickly, and only a card that
    // answers is worth the longer try at mounting it
    sdmmc_card_t *card = openRawCard(true);
    if (!card) {
        cardState = SD_ABSENT;
        return false;
    }
    sdmmc_host_deinit();
    if (mountCard()) return true;
    if (cardState != SD_UNREADABLE) cardUnreadable(card);
    return false;
}

CardState storageCardState() { return cardState; }
uint32_t storageMounts() { return mounts; }

uint64_t storageFreeBytes() { return mounted ? freeBytes : 0; }
uint64_t storageTotalBytes() { return mounted ? totalBytes : 0; }

static uint8_t *putChunkHeader(uint8_t *p, const char *id, uint32_t size) {
    memcpy(p, id, 4);
    memcpy(p + 4, &size, 4);
    return p + 8;
}

size_t storageWavHeader(const Moment &moment, uint8_t *buf) {
    uint32_t dataSize = moment.length * sizeof(int16_t);
    uint8_t *p = buf;
    p = putChunkHeader(p, "RIFF", WAV_HEADER_BYTES - 8 + dataSize);
    memcpy(p, "WAVE", 4);
    p += 4;

    WavFormat fmt = {1, 1, SAMPLE_RATE, SAMPLE_RATE * 2, 2, 16};
    p = putChunkHeader(p, "fmt ", sizeof(fmt));
    memcpy(p, &fmt, sizeof(fmt));
    p += sizeof(fmt);

    BextChunk bext = {};
    if (moment.micGainDb) {
        snprintf(bext.description, sizeof(bext.description), "Zvukopis Moment %d; mic gain %d dB%s; %d Hz 16-bit", moment.number,
                 moment.micGainDb, moment.micGainAuto ? " (auto)" : "", SAMPLE_RATE);
    } else { // a recording found again without its header: what gain it was made with is not known
        snprintf(bext.description, sizeof(bext.description), "Zvukopis Moment %d; %d Hz 16-bit", moment.number, SAMPLE_RATE);
    }
    strncpy(bext.originator, "Zvukopis", sizeof(bext.originator));
    snprintf(bext.originatorReference, sizeof(bext.originatorReference), "zvukopis-%04d", moment.number);
    if (moment.recordedAt) {
        tm local;
        localtime_r(&moment.recordedAt, &local);
        char date[11], time[9];
        strftime(date, sizeof(date), "%Y-%m-%d", &local);
        strftime(time, sizeof(time), "%H:%M:%S", &local);
        memcpy(bext.originationDate, date, 10);
        memcpy(bext.originationTime, time, 8);
        uint64_t ms = (uint64_t)(local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec) * 1000 + moment.startMs;
        uint64_t samples = ms * SAMPLE_RATE / 1000;
        bext.timeReferenceLow = (uint32_t)samples;
        bext.timeReferenceHigh = (uint32_t)(samples >> 32);
    }
    bext.version = 1;
    p = putChunkHeader(p, "bext", sizeof(bext));
    memcpy(p, &bext, sizeof(bext));
    p += sizeof(bext);

    MomentChunk extra = {MOMENT_CHUNK_VERSION, moment.number, moment.recordedAt, moment.gain};
    memcpy(extra.waveform, moment.waveform, WAVEFORM_BINS);
    extra.tag = moment.tag;
    extra.startMs = moment.startMs;
    p = putChunkHeader(p, "zvkp", sizeof(extra));
    memset(p, 0, EXTRA_PADDED);
    memcpy(p, &extra, sizeof(extra));
    p += EXTRA_PADDED;

    p = putChunkHeader(p, "data", dataSize);
    return p - buf;
}

bool storageSaveMoment(Moment &moment, const int16_t *samples) {
    if (!mounted) return false;
    chooseFileName(moment);
    char path[96];
    momentPath(path, sizeof(path), moment);
    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f) {
        Serial.printf("Can't create %s\n", path);
        return false;
    }

    uint8_t header[WAV_HEADER_BYTES];
    size_t headerSize = storageWavHeader(moment, header);
    uint32_t dataSize = moment.length * sizeof(int16_t);
    f.write(header, headerSize);
    size_t written = f.write((const uint8_t *)samples, dataSize);
    f.close();

    if (written != dataSize) {
        Serial.printf("Short write to %s (%u of %u bytes)\n", path, written, dataSize);
        SD_MMC.remove(path);
        moment.fileName[0] = 0;
        updateSpace();
        return false;
    }
    moment.dataOffset = headerSize;
    updateSpace(); // from the file system: an open file's size reads as 0 until it is closed
    return true;
}

// --- Recording straight onto the card ---

static const size_t RING_SAMPLES = 4 * SAMPLE_RATE; // 4 s of slack for the card's pauses
static const size_t WRITE_SAMPLES = 8192; // 16 KB per write (85 ms at 96 kHz): internal RAM, which the radio needs too
// The card driver sends a block in one go only from a buffer on a 4-byte
// boundary, and the file system hands it whole sectors only when the file
// stands at a sector boundary; anything else goes out one sector at a time,
// through a copy. After the 762-byte header every block would miss both. So
// the first block is shorter by the header: it brings the file to 16 KB, and
// each later one starts at a multiple of that, straight from the write buffer.
static const size_t FIRST_WRITE_SAMPLES = WRITE_SAMPLES - WAV_HEADER_BYTES / sizeof(int16_t);
// A file's length and its place on the card reach the card only with a sync.
// This often, so that a power cut costs the last seconds and no more.
static const size_t SYNC_SAMPLES = 5 * SAMPLE_RATE;
static int16_t *ring = nullptr;                     // PSRAM
static int16_t *writeBuffer = nullptr;              // internal RAM, which the card driver can DMA from
static volatile size_t ringHead = 0, ringTail = 0;  // in samples: head moved by the audio task, tail by the writer
static File streamFile;
// streamOpen: the file is the writer's to write to. The audio task sets it
// once the file is ready and clears it before touching the file again.
static volatile bool streamOpen = false, streaming = false, drainWanted = false, streamFailed = false;
static volatile size_t streamWritten = 0, streamSynced = 0, streamDropped = 0;
static TaskHandle_t writerTask = nullptr;
static SemaphoreHandle_t streamDrained;

static size_t ringAvailable() {
    size_t h = ringHead, t = ringTail;
    return h >= t ? h - t : RING_SAMPLES - t + h;
}

// Nothing here opens a file or looks one up by name: those calls go deepest
// into the card code, and this task's stack is sized for writing only
static void writerLoop(void *) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        bool finishing = !streaming;
        while (streamOpen && !streamFailed) {
            bool first = streamWritten == 0;
            size_t block = first ? FIRST_WRITE_SAMPLES : WRITE_SAMPLES;
            size_t available = ringAvailable();
            if (available == 0 || (available < block && !finishing)) break;
            size_t n = min(available, block), tail = ringTail;
            for (size_t i = 0; i < n; i++) writeBuffer[i] = ring[(tail + i) % RING_SAMPLES];
            size_t wrote = streamFile.write((const uint8_t *)writeBuffer, n * sizeof(int16_t)) / sizeof(int16_t);
            ringTail = (tail + wrote) % RING_SAMPLES;
            streamWritten += wrote;
            if (wrote < n) {
                Serial.println("Writing to the SD card failed; the recording stops here");
                streamFailed = true;
            } else if (!streamFailed && (first || streamWritten - streamSynced >= SYNC_SAMPLES)) {
                // After the first block this also empties the C library's own
                // buffer, which took the end of that block; whole blocks pass it by.
                // Not once End has given up waiting for this task: the file is its then.
                streamFile.flush();
                streamSynced = streamWritten;
            }
        }
        if (finishing && drainWanted && (ringAvailable() == 0 || streamFailed)) {
            drainWanted = false;
            xSemaphoreGive(streamDrained);
        }
    }
}

// The ring, the write buffer and the writer task serve every recording onto
// the card. False when one of them can't be had.
static bool streamReady() {
    if (!ring) ring = (int16_t *)ps_malloc(RING_SAMPLES * sizeof(int16_t));
    if (!writeBuffer) writeBuffer = (int16_t *)heap_caps_malloc(WRITE_SAMPLES * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!streamDrained) streamDrained = xSemaphoreCreateBinary();
    if (!ring || !writeBuffer || !streamDrained) return false;
    // Core 1: core 0 has the radio and the audio
    if (!writerTask && xTaskCreatePinnedToCore(writerLoop, "sdwriter", 4096, NULL, 2, &writerTask, 1) != pdPASS) writerTask = nullptr;
    return writerTask != nullptr;
}

bool storageStreamBegin(Moment &moment) {
    if (!mounted || !streamReady()) return false;
    chooseFileName(moment);
    char path[96];
    momentPath(path, sizeof(path), moment);
    streamFile = SD_MMC.open(path, FILE_WRITE);
    if (!streamFile) {
        Serial.printf("Can't create %s\n", path);
        moment.fileName[0] = 0;
        return false;
    }
    // The header goes in at once and is synced, so that the file is a moment
    // file from its first second, whatever happens to the power. It says the
    // longest a recording can be: a file cut off is known at the next start
    // by a header that says more than the file holds, and gets its real
    // length then (storageLoadMoments). Gain 0 marks it as not analysed.
    Moment provisional = moment;
    provisional.length = (size_t)MAX_RECORDING_SECONDS * SAMPLE_RATE;
    provisional.gain = 0;
    size_t headerSize = storageWavHeader(provisional, (uint8_t *)writeBuffer); // the writer is idle, its buffer is free
    bool written = streamFile.write((const uint8_t *)writeBuffer, headerSize) == headerSize;
    streamFile.flush();
    if (!written || streamFile.size() != headerSize) { // the size as the card's folder has it now
        Serial.printf("Can't write to %s\n", path);
        streamFile.close();
        SD_MMC.remove(path);
        moment.fileName[0] = 0;
        return false;
    }
    xSemaphoreTake(streamDrained, 0); // a late answer from a writer that was given up on
    ringHead = ringTail = 0;
    streamWritten = streamSynced = streamDropped = 0;
    streamFailed = drainWanted = false;
    streamOpen = true;
    streaming = true;
    return true;
}

size_t storageStreamWrite(const int16_t *samples, size_t n) {
    if (!streaming) return 0;
    size_t room = RING_SAMPLES - 1 - ringAvailable();
    size_t take = min(n, room);
    size_t head = ringHead;
    for (size_t i = 0; i < take; i++) ring[(head + i) % RING_SAMPLES] = samples[i];
    ringHead = (head + take) % RING_SAMPLES;
    streamDropped += n - take;
    if (writerTask) xTaskNotifyGive(writerTask);
    return take;
}

bool storageStreamFailed() { return streamFailed; }

bool storageStreamEnd(Moment &moment) {
    if (!streaming) return false;
    streaming = false;
    drainWanted = true;
    xTaskNotifyGive(writerTask);
    if (xSemaphoreTake(streamDrained, pdMS_TO_TICKS(10000)) != pdTRUE) {
        Serial.println("The SD card writer didn't finish in time");
        streamFailed = true; // what it is stuck on is given up, like a write that failed
    }
    streamOpen = false;
    moment.length = streamWritten;
    moment.dataOffset = WAV_HEADER_BYTES;
    char path[96];
    momentPath(path, sizeof(path), moment);
    if (streamFailed) {
        // A file handle that has met a card error takes nothing more, though
        // closing it still records the file's length. A fresh one tells how
        // much reached the card, and can take the header: that overwrites
        // what is there and needs no room, so it works on a full card too.
        streamFile.close();
        streamFile = SD_MMC.open(path, "r+");
        size_t fileSize = streamFile ? streamFile.size() : 0;
        moment.length = fileSize > WAV_HEADER_BYTES ? (fileSize - WAV_HEADER_BYTES) / sizeof(int16_t) : 0;
    }
    bool reached = streamFile, ok = reached && moment.length > 0;
    if (ok) {
        uint8_t *header = (uint8_t *)writeBuffer; // the writer is done with it
        size_t headerSize = storageWavHeader(moment, header);
        // Should this fail, the header stays as Begin wrote it and the next start puts it right
        if (!streamFile.seek(0) || streamFile.write(header, headerSize) != headerSize) Serial.printf("Couldn't finish the header of %s\n", path);
    }
    if (reached) streamFile.close();
    if (streamDropped) Serial.printf("The SD card fell behind: %u samples lost\n", streamDropped);
    if (ok) renameToMatch(moment); // named when the file was made; the first sample may have come in the next second
    else if (reached) storageDeleteMoment(moment); // nothing in it but the header
    updateSpace(); // from the file system: an open file's size reads as 0 until it is closed
    return ok;
}

// Walks the chunks of an open moment file, calling `found` with each chunk's
// id, size and payload position until it returns true. Every step has to lead
// further on inside the file, and a moment file has its samples within its
// first few chunks: a size that says otherwise (a damaged file, or a made-up
// one) ends the walk, which could otherwise go in circles and never end.
static const int MAX_CHUNKS = 32;

template <typename F>
static bool forEachChunk(File &f, F found) {
    char id[4];
    uint32_t size, fileSize = f.size();
    f.seek(0);
    if (f.read((uint8_t *)id, 4) != 4 || memcmp(id, "RIFF", 4) != 0) return false;
    f.read((uint8_t *)&size, 4);
    if (f.read((uint8_t *)id, 4) != 4 || memcmp(id, "WAVE", 4) != 0) return false;
    for (int chunk = 0; chunk < MAX_CHUNKS && f.read((uint8_t *)id, 4) == 4 && f.read((uint8_t *)&size, 4) == 4; chunk++) {
        uint32_t payload = f.position();
        if (found(id, size, payload)) return true;
        uint64_t next = (uint64_t)payload + size + (size & 1);
        if (next + 8 > fileSize || !f.seek((uint32_t)next)) return false;
    }
    return false;
}

// What the start of a file in the moments folder turned out to be
enum HeaderKind {
    HEADER_FOREIGN,  // not a moment file
    HEADER_COMPLETE,
    HEADER_CUT_OFF,  // a recording that never got its ending: the header still says more than the file holds
    HEADER_NO_SOUND, // ... and holds no samples at all
};

// Reads the header of one moment file. Accepts only what this firmware writes.
static HeaderKind readMomentHeader(File &f, Moment &moment) {
    bool haveFormat = false, haveExtra = false, cutOff = false;
    moment = {};
    strlcpy(moment.fileName, f.name(), sizeof(moment.fileName));
    bool haveData = forEachChunk(f, [&](const char *id, uint32_t size, uint32_t payload) {
        if (memcmp(id, "fmt ", 4) == 0 && size >= sizeof(WavFormat)) {
            WavFormat fmt;
            f.read((uint8_t *)&fmt, sizeof(fmt));
            haveFormat = fmt.audioFormat == 1 && fmt.channels == 1 && fmt.bitsPerSample == 16 &&
                         fmt.sampleRate >= 8000 && fmt.sampleRate <= 192000;
            moment.sampleRate = fmt.sampleRate;
        } else if (memcmp(id, "zvkp", 4) == 0 && size >= sizeof(MomentChunk)) {
            MomentChunk extra;
            f.read((uint8_t *)&extra, sizeof(extra));
            moment.number = extra.number;
            moment.recordedAt = extra.recordedAt;
            moment.gain = extra.gain;
            memcpy(moment.waveform, extra.waveform, WAVEFORM_BINS);
            moment.tag = extra.tag <= CATEGORY_MAX ? extra.tag : TAG_NONE;
            moment.startMs = extra.startMs;
            haveExtra = true;
        } else if (memcmp(id, "data", 4) == 0) {
            uint32_t held = f.size() - payload;
            cutOff = size > held;
            moment.dataOffset = payload;
            moment.length = min(size, held) / sizeof(int16_t);
            return true;
        }
        return false;
    });
    if (!haveData || !haveFormat || !haveExtra) return HEADER_FOREIGN;
    if (moment.length == 0) return cutOff ? HEADER_NO_SOUND : HEADER_FOREIGN;
    return cutOff ? HEADER_CUT_OFF : HEADER_COMPLETE;
}

// Firmware from before the header was written first left it as zeros until a
// recording ended. One that never ended has no length on the card either, so
// such a file only turns up again after a repair of the card on a computer.
static bool blankHeader(File &f) {
    if (f.size() <= WAV_HEADER_BYTES || !f.seek(0)) return false;
    uint8_t part[64];
    for (size_t done = 0; done < WAV_HEADER_BYTES; done += sizeof(part)) {
        size_t n = min(sizeof(part), WAV_HEADER_BYTES - done);
        if (f.read(part, n) != n) return false;
        for (size_t i = 0; i < n; i++) {
            if (part[i]) return false;
        }
    }
    return true;
}

// A moment without a header has no number either: it gets one once the
// highest number on the card is known (storageLoadMoments)
static const int NUMBER_WANTED = INT32_MIN;

// The start time in a moment file's name ("2026-10-28T10-05-15..."), 0 when it has none
static time_t timeFromName(const char *name) {
    tm local = {};
    if (sscanf(name, "%4d-%2d-%2dT%2d-%2d-%2d", &local.tm_year, &local.tm_mon, &local.tm_mday, &local.tm_hour, &local.tm_min,
               &local.tm_sec) != 6) {
        return 0;
    }
    local.tm_year -= 1900;
    local.tm_mon -= 1;
    local.tm_isdst = -1;
    return mktime(&local);
}

// Writes the lengths a cut-off recording really has into its header
static bool writeLengths(const char *path, const Moment &moment) {
    File f = SD_MMC.open(path, "r+");
    if (!f) return false;
    uint32_t dataSize = moment.length * sizeof(int16_t), riffSize = moment.dataOffset - 8 + dataSize;
    bool ok = f.seek(4) && f.write((const uint8_t *)&riffSize, 4) == 4 && f.seek(moment.dataOffset - 4) &&
              f.write((const uint8_t *)&dataSize, 4) == 4;
    f.close();
    return ok;
}

// Writes a whole new header over the first WAV_HEADER_BYTES of a file
static bool writeHeader(const char *path, const Moment &moment) {
    uint8_t *header = (uint8_t *)malloc(WAV_HEADER_BYTES); // not on the stack, which the card code is about to go deep into
    File f = SD_MMC.open(path, "r+");
    bool ok = header && f && f.write(header, storageWavHeader(moment, header)) == WAV_HEADER_BYTES;
    if (f) f.close();
    free(header);
    return ok;
}

// One file of the moments folder into `moment`; false when it isn't a moment.
// A recording that was cut off gets its real length on the way.
static bool loadMoment(File &f, const char *folder, Moment &moment) {
    HeaderKind kind = readMomentHeader(f, moment);
    strlcpy(moment.folder, folder, sizeof(moment.folder));
    char path[96];
    momentPath(path, sizeof(path), moment);
    if (kind == HEADER_COMPLETE) return true;
    if (kind == HEADER_CUT_OFF) {
        f.close(); // one handle on the file at a time
        bool written = writeLengths(path, moment);
        Serial.printf("[storage] recovered %s: %.1f s of a recording that was cut off%s\n", path,
                      (float)moment.length / moment.sampleRate, written ? "" : " (its header couldn't be put right)");
        return true;
    }
    if (kind == HEADER_FOREIGN && blankHeader(f)) {
        size_t length = (f.size() - WAV_HEADER_BYTES) / sizeof(int16_t);
        moment = {};
        moment.number = NUMBER_WANTED;
        moment.recordedAt = timeFromName(f.name());
        moment.sampleRate = SAMPLE_RATE; // nothing says; this firmware's own is the likeliest
        moment.dataOffset = WAV_HEADER_BYTES;
        moment.length = length;
        strlcpy(moment.fileName, f.name(), sizeof(moment.fileName));
        strlcpy(moment.folder, folder, sizeof(moment.folder));
        return true;
    }
    Serial.printf("Skipping %s: %s\n", path,
                  kind == HEADER_NO_SOUND ? "a recording cut off before any sound was on the card" : "not a moment file");
    return false;
}

// The moment files in one folder ("" for /moments itself, from before the day
// folders). Once `out` is full, the files left over are only counted, into `unlisted`.
static int loadFolder(const char *folder, Moment *out, int max, int &unlisted) {
    char path[32];
    snprintf(path, sizeof(path), folder[0] ? "%s/%s" : "%s%s", MOMENT_DIR, folder);
    File dir = SD_MMC.open(path);
    if (!dir) return 0;
    int count = 0;
    while (count < max) {
        File f = dir.openNextFile();
        if (!f) break;
        // f.name() is the bare file name in this core version
        const char *dot = strrchr(f.name(), '.');
        // A file with nothing in it is left alone, without a word: a recording that older firmware lost to a power cut
        if (!f.isDirectory() && dot && strcasecmp(dot, ".wav") == 0 && f.size() > 0) {
            if (strlen(f.name()) >= sizeof(out->fileName)) {
                Serial.printf("Skipping %s/%s: its name is too long (%u bytes at most)\n", path, f.name(),
                              (unsigned)sizeof(out->fileName) - 1);
            } else if (loadMoment(f, folder, out[count])) {
                count++;
            }
        }
        f.close();
    }
    if (count >= max) {
        // By name only: opening each file would take as long as loading it
        bool isDirectory;
        for (String name = dir.getNextFileName(&isDirectory); name.length(); name = dir.getNextFileName(&isDirectory)) {
            name.toLowerCase();
            if (!isDirectory && name.endsWith(".wav")) unlisted++;
        }
    }
    dir.close();
    return count;
}

int storageLoadMoments(Moment *out, int max) {
    if (!mounted) return 0;
    uint32_t start = millis();
    int unlisted = 0;
    int count = loadFolder("", out, max, unlisted);
    File dir = SD_MMC.open(MOMENT_DIR);
    for (File f = dir ? dir.openNextFile() : File(); f; f = dir.openNextFile()) {
        if (f.isDirectory() && strlen(f.name()) < sizeof(out->folder)) count += loadFolder(f.name(), out + count, max - count, unlisted);
        f.close();
    }
    if (dir) dir.close();

    int highest = 0;
    for (int i = 0; i < count; i++) {
        if (out[i].number != NUMBER_WANTED && out[i].number > highest) highest = out[i].number;
    }
    for (int i = 0; i < count; i++) {
        if (out[i].number != NUMBER_WANTED) continue;
        out[i].number = ++highest;
        char path[96];
        momentPath(path, sizeof(path), out[i]);
        bool written = writeHeader(path, out[i]);
        Serial.printf("[storage] recovered %s as Moment %d: %.1f s of a recording left without a header%s\n", path, out[i].number,
                      (float)out[i].length / out[i].sampleRate, written ? "" : " (it couldn't be given one)");
    }
    Serial.printf("Loaded %d moments from SD card in %lu ms\n", count, millis() - start);
    if (unlisted) Serial.printf("The list holds %d moments: %d more files on the card are not listed\n", MAX_MOMENTS, unlisted);
    return count;
}

bool storageDeleteMoment(const Moment &moment) {
    if (!mounted) return false;
    char path[96];
    momentPath(path, sizeof(path), moment);
    if (!SD_MMC.remove(path)) return false;
    if (moment.folder[0]) { // the day's folder goes once it's empty (rmdir fails otherwise, which is fine)
        char folder[32];
        folderPath(folder, sizeof(folder), moment);
        SD_MMC.rmdir(folder);
    }
    updateSpace();
    return true;
}

// Gives the moment's file the name its time and tag make, if it has another
static void renameToMatch(Moment &moment) {
    Moment renamed = moment;
    storageMomentFileName(renamed, renamed.fileName, sizeof(renamed.fileName));
    if (strcmp(renamed.fileName, moment.fileName) == 0) return;
    char path[96], newPath[96];
    momentPath(path, sizeof(path), moment);
    momentPath(newPath, sizeof(newPath), renamed);
    if (SD_MMC.exists(newPath)) {
        char *dot = strrchr(renamed.fileName, '.');
        if (dot) snprintf(dot, sizeof(renamed.fileName) - (dot - renamed.fileName), "_%d.wav", renamed.number);
        momentPath(newPath, sizeof(newPath), renamed);
        if (strcmp(path, newPath) == 0) return; // it has that name because the plain one was taken
    }
    if (!SD_MMC.rename(path, newPath)) {
        Serial.printf("Couldn't rename %s to %s\n", path, newPath);
        return;
    }
    strlcpy(moment.fileName, renamed.fileName, sizeof(moment.fileName));
}

bool storageUpdateMoment(Moment &moment) {
    if (!mounted) return false;
    char path[96];
    momentPath(path, sizeof(path), moment);
    File f = SD_MMC.open(path, "r+");
    if (!f) return false;
    bool ok = forEachChunk(f, [&](const char *id, uint32_t size, uint32_t payload) {
        if (memcmp(id, "zvkp", 4) != 0 || size < sizeof(MomentChunk)) return false;
        // The number is new when it collided with another file's; gain and
        // waveform when the recording was cut off and they were worked out since
        MomentChunk extra;
        if (f.read((uint8_t *)&extra, sizeof(extra)) != sizeof(extra)) return false;
        extra.number = moment.number;
        extra.gain = moment.gain;
        memcpy(extra.waveform, moment.waveform, WAVEFORM_BINS);
        extra.tag = moment.tag;
        return f.seek(payload) && f.write((const uint8_t *)&extra, sizeof(extra)) == sizeof(extra);
    });
    f.close();
    if (!ok) return false;

    renameToMatch(moment); // the name carries the tag, so it changes with it; if it can't, the tag is in the file anyway
    return true;
}

File storageOpenMoment(const Moment &moment) {
    char path[96];
    momentPath(path, sizeof(path), moment);
    File f = SD_MMC.open(path);
    if (f) f.seek(moment.dataOffset);
    return f;
}

// --- Whole-card access, with the file system unmounted ---

static sdmmc_card_t rawCard;

static sdmmc_card_t *openRawCard(bool quiet) {
    if (mounted) {
        SD_MMC.end();
        mounted = false;
        mounts = mounts + 1;
        cardState = SD_ABSENT;
    }
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = (gpio_num_t)SD_CLK_PIN;
    slot.cmd = (gpio_num_t)SD_CMD_PIN;
    slot.d0 = (gpio_num_t)SD_D0_PIN;
    slot.d1 = (gpio_num_t)SD_D1_PIN;
    slot.d2 = (gpio_num_t)SD_D2_PIN;
    slot.d3 = (gpio_num_t)SD_D3_PIN;
    slot.width = 4;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = sdmmc_host_init();
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) err = sdmmc_host_init_slot(host.slot, &slot);
    if (err == ESP_OK) err = sdmmc_card_init(&host, &rawCard);
    if (err != ESP_OK) {
        if (!quiet) Serial.printf("SD card init failed (0x%x)\n", err);
        sdmmc_host_deinit();
        return nullptr;
    }
    return &rawCard;
}

sdmmc_card_t *storageOpenRawCard() {
    sdmmc_card_t *card = openRawCard();
    cardLent = card != nullptr; // the caller's from here on: storageRecheck() must not mount it under them
    return card;
}

bool storageFormat() {
    sdmmc_card_t *card = openRawCard();
    if (!card) {
        storageInit();
        return false;
    }

    BYTE pdrv = 0xFF;
    bool ok = ff_diskio_get_drive(&pdrv) == ESP_OK && pdrv != 0xFF;
    if (ok) {
        ff_diskio_register_sdmmc(pdrv, card);
        char drive[3] = {(char)('0' + pdrv), ':', 0};
        const size_t workSize = 4096;
        void *work = malloc(workSize);
        uint32_t start = millis();
        FRESULT res = work ? f_mkfs(drive, FM_ANY, 0, work, workSize) : FR_NOT_ENOUGH_CORE;
        free(work);
        ff_diskio_register(pdrv, NULL);
        Serial.printf("Formatting the SD card %s in %lu ms (FatFs result %d)\n",
                      res == FR_OK ? "done" : "failed", millis() - start, res);
        ok = res == FR_OK;
    }
    sdmmc_host_deinit();
    return storageInit() && ok;
}

static void listDir(const char *path, int depth) {
    File dir = SD_MMC.open(path);
    if (!dir) {
        Serial.printf("%*s(can't open %s)\n", depth * 2, "", path);
        return;
    }
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        if (f.isDirectory()) {
            Serial.printf("%*s%s/\n", depth * 2, "", f.name());
            char sub[128];
            snprintf(sub, sizeof(sub), "%s/%s", strcmp(path, "/") == 0 ? "" : path, f.name());
            if (depth < 4) listDir(sub, depth + 1);
        } else {
            Serial.printf("%*s%s  %lu bytes\n", depth * 2, "", f.name(), (unsigned long)f.size());
        }
        f.close();
    }
    dir.close();
}

void storageListFiles() {
    if (!mounted) {
        Serial.println("[storage] no card mounted");
        return;
    }
    Serial.println("[storage] files on the card:");
    listDir("/", 0);
}
