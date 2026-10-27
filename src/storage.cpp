#include "storage.h"

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

static void momentPath(char *buf, size_t len, const char *fileName) {
    snprintf(buf, len, "%s/%s", MOMENT_DIR, fileName);
}

// Local start time if known, else the moment number; made unique if taken
static void chooseFileName(Moment &moment) {
    char base[24], path[48];
    if (moment.recordedAt) {
        tm local;
        localtime_r(&moment.recordedAt, &local);
        strftime(base, sizeof(base), "%Y-%m-%d_%H-%M-%S", &local);
    } else {
        snprintf(base, sizeof(base), "MOMENT_%04d", moment.number);
    }
    snprintf(moment.fileName, sizeof(moment.fileName), "%s.wav", base);
    momentPath(path, sizeof(path), moment.fileName);
    if (SD_MMC.exists(path)) snprintf(moment.fileName, sizeof(moment.fileName), "%s_%d.wav", base, moment.number);
}

static void updateSpace() {
    totalBytes = SD_MMC.totalBytes();
    freeBytes = totalBytes - SD_MMC.usedBytes();
}

bool storageInit() {
    SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN, SD_D1_PIN, SD_D2_PIN, SD_D3_PIN);
    mounted = SD_MMC.begin(MOUNT_POINT, false);
    if (!mounted) {
        Serial.println("No SD card, moments are kept in memory only");
        return false;
    }
    if (!SD_MMC.exists(MOMENT_DIR)) SD_MMC.mkdir(MOMENT_DIR);
    updateSpace();
    Serial.printf("SD card mounted: %llu MB, %llu MB free\n", SD_MMC.cardSize() >> 20, freeBytes >> 20);
    return true;
}

bool storageAvailable() { return mounted; }

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
    snprintf(bext.description, sizeof(bext.description), "zvukopis Moment %d", moment.number);
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
    char path[48];
    momentPath(path, sizeof(path), moment.fileName);
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

int storageLoadMoments(Moment *out, int max) {
    if (!mounted) return 0;
    File dir = SD_MMC.open(MOMENT_DIR);
    if (!dir) return 0;
    int count = 0;
    uint32_t start = millis();
    for (File f = dir.openNextFile(); f && count < max; f = dir.openNextFile()) {
        // f.name() is the bare file name in this core version
        const char *dot = strrchr(f.name(), '.');
        if (!f.isDirectory() && dot && strcasecmp(dot, ".wav") == 0) {
            if (readMomentHeader(f, out[count])) count++;
            else Serial.printf("Skipping %s: not a moment file\n", f.name());
        }
        f.close();
    }
    dir.close();
    Serial.printf("Loaded %d moments from SD card in %lu ms\n", count, millis() - start);
    return count;
}

bool storageDeleteMoment(const Moment &moment) {
    if (!mounted) return false;
    char path[48];
    momentPath(path, sizeof(path), moment.fileName);
    File f = SD_MMC.open(path);
    size_t fileSize = f ? f.size() : 0;
    f.close();
    if (!SD_MMC.remove(path)) return false;
    freeBytes += fileSize;
    return true;
}

bool storageUpdateMoment(const Moment &moment) {
    if (!mounted) return false;
    char path[48];
    momentPath(path, sizeof(path), moment.fileName);
    File f = SD_MMC.open(path, "r+");
    if (!f) return false;
    bool ok = forEachChunk(f, [&](const char *id, uint32_t size, uint32_t payload) {
        if (memcmp(id, "zvkp", 4) != 0 || size < sizeof(MomentChunk)) return false;
        f.seek(payload + offsetof(MomentChunk, tag));
        f.write(&moment.tag, 1);
        return true;
    });
    f.close();
    return ok;
}

File storageOpenMoment(const Moment &moment) {
    char path[48];
    momentPath(path, sizeof(path), moment.fileName);
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
