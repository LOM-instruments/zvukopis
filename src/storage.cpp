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
static CardState cardState = SD_ABSENT;
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

bool storageInit() {
    SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN, SD_D1_PIN, SD_D2_PIN, SD_D3_PIN);
    mounted = SD_MMC.begin(MOUNT_POINT, false);
    if (!mounted) {
        sdmmc_card_t *card = storageOpenRawCard();
        if (card) {
            cardState = SD_UNREADABLE;
            Serial.printf("SD card found (%llu MB) but its file system can't be read (exFAT?): format it in Settings\n",
                          (uint64_t)card->csd.capacity * card->csd.sector_size >> 20);
            sdmmc_host_deinit();
        } else {
            cardState = SD_ABSENT;
            Serial.println("No SD card, moments are kept in memory only");
        }
        return false;
    }
    cardState = SD_MOUNTED;
    if (!SD_MMC.exists(MOMENT_DIR)) SD_MMC.mkdir(MOMENT_DIR);
    updateSpace();
    Serial.printf("SD card mounted: %llu MB, %llu MB free\n", SD_MMC.cardSize() >> 20, freeBytes >> 20);
    return true;
}

bool storageAvailable() { return mounted; }
CardState storageCardState() { return cardState; }

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
    snprintf(bext.description, sizeof(bext.description), "zvukopis Moment %d; mic gain %d dB%s; %d Hz 16-bit", moment.number,
             moment.micGainDb, moment.micGainAuto ? " (auto)" : "", SAMPLE_RATE);
    strncpy(bext.originator, "zvukopis", sizeof(bext.originator));
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
    size_t fileSize = f.size();
    f.close();

    if (written != dataSize) {
        Serial.printf("Short write to %s (%u of %u bytes)\n", path, written, dataSize);
        SD_MMC.remove(path);
        moment.fileName[0] = 0;
        return false;
    }
    moment.dataOffset = headerSize;
    freeBytes -= min((uint64_t)fileSize, freeBytes);
    return true;
}

// --- Recording straight onto the card ---

static const size_t RING_SAMPLES = 4 * SAMPLE_RATE; // 4 s of slack for the card's pauses
static const size_t WRITE_SAMPLES = 12 * 1024;      // 24 KB per card write
static int16_t *ring = nullptr;                     // PSRAM
static int16_t *writeBuffer = nullptr;              // internal RAM, which the card driver can DMA from
static volatile size_t ringHead = 0, ringTail = 0;  // in samples: head moved by the audio task, tail by the writer
static File streamFile;
static volatile bool streaming = false, drainWanted = false, streamFailed = false;
static volatile size_t streamWritten = 0, streamDropped = 0;
static TaskHandle_t writerTask = nullptr;
static SemaphoreHandle_t streamDrained;

static size_t ringAvailable() {
    size_t h = ringHead, t = ringTail;
    return h >= t ? h - t : RING_SAMPLES - t + h;
}

static void writerLoop(void *) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        bool finishing = !streaming;
        while (streamFile && !streamFailed) {
            size_t available = ringAvailable();
            if (available == 0 || (available < WRITE_SAMPLES && !finishing)) break;
            size_t n = min(available, WRITE_SAMPLES), tail = ringTail;
            for (size_t i = 0; i < n; i++) writeBuffer[i] = ring[(tail + i) % RING_SAMPLES];
            size_t wrote = streamFile.write((const uint8_t *)writeBuffer, n * sizeof(int16_t)) / sizeof(int16_t);
            ringTail = (tail + wrote) % RING_SAMPLES;
            streamWritten += wrote;
            if (wrote < n) {
                Serial.println("Writing to the SD card failed; the recording stops here");
                streamFailed = true;
            }
        }
        if (finishing && drainWanted && (ringAvailable() == 0 || streamFailed)) {
            drainWanted = false;
            xSemaphoreGive(streamDrained);
        }
    }
}

bool storageStreamBegin(Moment &moment) {
    if (!mounted) return false;
    if (!ring) ring = (int16_t *)ps_malloc(RING_SAMPLES * sizeof(int16_t));
    if (!writeBuffer) writeBuffer = (int16_t *)heap_caps_malloc(WRITE_SAMPLES * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!streamDrained) streamDrained = xSemaphoreCreateBinary();
    if (!ring || !writeBuffer || !streamDrained) return false;
    chooseFileName(moment);
    char path[96];
    momentPath(path, sizeof(path), moment);
    streamFile = SD_MMC.open(path, FILE_WRITE);
    if (!streamFile) {
        Serial.printf("Can't create %s\n", path);
        return false;
    }
    uint8_t blank[WAV_HEADER_BYTES] = {}; // the real header goes in at the end, once the sizes are known
    streamFile.write(blank, sizeof(blank));
    ringHead = ringTail = 0;
    streamWritten = streamDropped = 0;
    streamFailed = drainWanted = false;
    streaming = true;
    if (!writerTask) xTaskCreatePinnedToCore(writerLoop, "sdwriter", 4096, NULL, 2, &writerTask, 0);
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

bool storageStreamEnd(Moment &moment) {
    if (!streaming) return false;
    streaming = false;
    drainWanted = true;
    xTaskNotifyGive(writerTask);
    if (xSemaphoreTake(streamDrained, pdMS_TO_TICKS(10000)) != pdTRUE) Serial.println("The SD card writer didn't finish in time");
    moment.length = streamWritten;
    moment.dataOffset = WAV_HEADER_BYTES;
    bool ok = !streamFailed && streamWritten > 0;
    if (ok) {
        uint8_t header[WAV_HEADER_BYTES];
        size_t headerSize = storageWavHeader(moment, header);
        ok = streamFile.seek(0) && streamFile.write(header, headerSize) == headerSize;
    }
    size_t fileSize = streamFile.size();
    streamFile.close();
    freeBytes -= min((uint64_t)fileSize, freeBytes);
    if (streamDropped) Serial.printf("The SD card fell behind: %u samples lost\n", streamDropped);
    return ok;
}

// Walks the chunks of an open moment file, calling `found` with each chunk's
// id, size and payload position until it returns true
template <typename F>
static bool forEachChunk(File &f, F found) {
    char id[4];
    uint32_t size;
    f.seek(0);
    if (f.read((uint8_t *)id, 4) != 4 || memcmp(id, "RIFF", 4) != 0) return false;
    f.read((uint8_t *)&size, 4);
    if (f.read((uint8_t *)id, 4) != 4 || memcmp(id, "WAVE", 4) != 0) return false;
    while (f.read((uint8_t *)id, 4) == 4 && f.read((uint8_t *)&size, 4) == 4) {
        uint32_t payload = f.position();
        if (found(id, size, payload)) return true;
        f.seek(payload + size + (size & 1));
    }
    return false;
}

// Reads the header of one moment file. Accepts only what storageSaveMoment writes.
static bool readMomentHeader(File &f, Moment &moment) {
    bool haveFormat = false, haveExtra = false;
    moment = {};
    strlcpy(moment.fileName, f.name(), sizeof(moment.fileName));
    bool haveData = forEachChunk(f, [&](const char *id, uint32_t size, uint32_t payload) {
        if (memcmp(id, "fmt ", 4) == 0 && size >= sizeof(WavFormat)) {
            WavFormat fmt;
            f.read((uint8_t *)&fmt, sizeof(fmt));
            haveFormat = fmt.audioFormat == 1 && fmt.channels == 1 &&
                         fmt.sampleRate == SAMPLE_RATE && fmt.bitsPerSample == 16;
        } else if (memcmp(id, "zvkp", 4) == 0 && size >= sizeof(MomentChunk)) {
            MomentChunk extra;
            f.read((uint8_t *)&extra, sizeof(extra));
            moment.number = extra.number;
            moment.recordedAt = extra.recordedAt;
            moment.gain = extra.gain;
            memcpy(moment.waveform, extra.waveform, WAVEFORM_BINS);
            moment.tag = extra.tag < TAG_COUNT ? extra.tag : TAG_NONE;
            moment.startMs = extra.startMs;
            haveExtra = true;
        } else if (memcmp(id, "data", 4) == 0) {
            moment.dataOffset = payload;
            moment.length = min(size, (uint32_t)(f.size() - payload)) / sizeof(int16_t);
            return true;
        }
        return false;
    });
    return haveData && haveFormat && haveExtra && moment.length > 0;
}

// The moment files in one folder ("" for /moments itself, from before the day folders)
static int loadFolder(const char *folder, Moment *out, int max) {
    char path[32];
    snprintf(path, sizeof(path), folder[0] ? "%s/%s" : "%s%s", MOMENT_DIR, folder);
    File dir = SD_MMC.open(path);
    if (!dir) return 0;
    int count = 0;
    for (File f = dir.openNextFile(); f && count < max; f = dir.openNextFile()) {
        // f.name() is the bare file name in this core version
        const char *dot = strrchr(f.name(), '.');
        if (!f.isDirectory() && dot && strcasecmp(dot, ".wav") == 0) {
            if (readMomentHeader(f, out[count])) {
                strlcpy(out[count].folder, folder, sizeof(out[count].folder));
                count++;
            } else {
                Serial.printf("Skipping %s/%s: not a moment file\n", folder, f.name());
            }
        }
        f.close();
    }
    dir.close();
    return count;
}

int storageLoadMoments(Moment *out, int max) {
    if (!mounted) return 0;
    uint32_t start = millis();
    int count = loadFolder("", out, max);
    File dir = SD_MMC.open(MOMENT_DIR);
    for (File f = dir ? dir.openNextFile() : File(); f && count < max; f = dir.openNextFile()) {
        if (f.isDirectory() && strlen(f.name()) < sizeof(out->folder)) count += loadFolder(f.name(), out + count, max - count);
        f.close();
    }
    if (dir) dir.close();
    Serial.printf("Loaded %d moments from SD card in %lu ms\n", count, millis() - start);
    return count;
}

bool storageDeleteMoment(const Moment &moment) {
    if (!mounted) return false;
    char path[96];
    momentPath(path, sizeof(path), moment);
    File f = SD_MMC.open(path);
    size_t fileSize = f ? f.size() : 0;
    f.close();
    if (!SD_MMC.remove(path)) return false;
    freeBytes += fileSize;
    if (moment.folder[0]) { // the day's folder goes once it's empty (rmdir fails otherwise, which is fine)
        char folder[32];
        folderPath(folder, sizeof(folder), moment);
        SD_MMC.rmdir(folder);
    }
    return true;
}

bool storageUpdateMoment(Moment &moment) {
    if (!mounted) return false;
    char path[96];
    momentPath(path, sizeof(path), moment);
    File f = SD_MMC.open(path, "r+");
    if (!f) return false;
    bool ok = forEachChunk(f, [&](const char *id, uint32_t size, uint32_t payload) {
        if (memcmp(id, "zvkp", 4) != 0 || size < sizeof(MomentChunk)) return false;
        f.seek(payload + offsetof(MomentChunk, tag));
        f.write(&moment.tag, 1);
        return true;
    });
    f.close();
    if (!ok) return false;

    // The name carries the tag, so it changes with it
    Moment renamed = moment;
    storageMomentFileName(renamed, renamed.fileName, sizeof(renamed.fileName));
    if (strcmp(renamed.fileName, moment.fileName) == 0) return true;
    char newPath[96];
    momentPath(newPath, sizeof(newPath), renamed);
    if (SD_MMC.exists(newPath)) {
        char *dot = strrchr(renamed.fileName, '.');
        if (dot) snprintf(dot, sizeof(renamed.fileName) - (dot - renamed.fileName), "_%d.wav", renamed.number);
        momentPath(newPath, sizeof(newPath), renamed);
    }
    if (!SD_MMC.rename(path, newPath)) {
        Serial.printf("Couldn't rename %s to %s\n", path, newPath);
        return true; // the tag is in the file anyway
    }
    strlcpy(moment.fileName, renamed.fileName, sizeof(moment.fileName));
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

sdmmc_card_t *storageOpenRawCard() {
    if (mounted) {
        SD_MMC.end();
        mounted = false;
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
        Serial.printf("SD card init failed (0x%x)\n", err);
        sdmmc_host_deinit();
        return nullptr;
    }
    return &rawCard;
}

bool storageFormat() {
    sdmmc_card_t *card = storageOpenRawCard();
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
