#include "storage.h"

#include <Arduino.h>
#include <SD_MMC.h>

// Same wiring as Waveshare's SD examples
static const int SD_CLK_PIN = 16;
static const int SD_CMD_PIN = 17;
static const int SD_D0_PIN = 15;
static const int SD_D1_PIN = 7;
static const int SD_D2_PIN = 8;
static const int SD_D3_PIN = 18;

static const char *MEMO_DIR = "/memos";
static const uint32_t MEMO_CHUNK_VERSION = 1;

static bool mounted = false;
static uint64_t freeBytes = 0;

// "memo" chunk payload
struct __attribute__((packed)) MemoChunk {
    uint32_t version;
    int32_t number;
    int64_t recordedAt; // UTC, 0 if unknown
    float gain;
    uint8_t waveform[WAVEFORM_BINS];
};

struct __attribute__((packed)) WavFormat {
    uint16_t audioFormat; // 1 = PCM
    uint16_t channels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
};

static void memoPath(char *buf, size_t len, const char *fileName) {
    snprintf(buf, len, "%s/%s", MEMO_DIR, fileName);
}

// Local start time if known, else the memo number; made unique if taken
static void chooseFileName(Memo &memo) {
    char base[24], path[48];
    if (memo.recordedAt) {
        tm local;
        localtime_r(&memo.recordedAt, &local);
        strftime(base, sizeof(base), "%Y-%m-%d_%H-%M-%S", &local);
    } else {
        snprintf(base, sizeof(base), "MEMO_%04d", memo.number);
    }
    snprintf(memo.fileName, sizeof(memo.fileName), "%s.wav", base);
    memoPath(path, sizeof(path), memo.fileName);
    if (SD_MMC.exists(path)) snprintf(memo.fileName, sizeof(memo.fileName), "%s_%d.wav", base, memo.number);
}

bool storageInit() {
    SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN, SD_D1_PIN, SD_D2_PIN, SD_D3_PIN);
    mounted = SD_MMC.begin("/sdcard", false);
    if (!mounted) {
        Serial.println("No SD card, memos are kept in memory only");
        return false;
    }
    if (!SD_MMC.exists(MEMO_DIR)) SD_MMC.mkdir(MEMO_DIR);
    freeBytes = SD_MMC.totalBytes() - SD_MMC.usedBytes();
    Serial.printf("SD card mounted: %llu MB, %llu MB free\n",
                  SD_MMC.cardSize() >> 20, freeBytes >> 20);
    return true;
}

bool storageAvailable() { return mounted; }

uint64_t storageFreeBytes() { return mounted ? freeBytes : 0; }

static void writeChunkHeader(File &f, const char *id, uint32_t size) {
    f.write((const uint8_t *)id, 4);
    f.write((const uint8_t *)&size, 4);
}

bool storageSaveMemo(Memo &memo, const int16_t *samples) {
    if (!mounted) return false;
    chooseFileName(memo);
    char path[48];
    memoPath(path, sizeof(path), memo.fileName);
    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f) {
        Serial.printf("Can't create %s\n", path);
        return false;
    }

    MemoChunk extra = {MEMO_CHUNK_VERSION, memo.number, memo.recordedAt, memo.gain};
    memcpy(extra.waveform, memo.waveform, WAVEFORM_BINS);
    const uint32_t extraSize = sizeof(extra);
    const uint32_t extraPadded = extraSize + (extraSize & 1); // RIFF chunks are word aligned
    WavFormat fmt = {1, 1, SAMPLE_RATE, SAMPLE_RATE * 2, 2, 16};
    uint32_t dataSize = memo.length * sizeof(int16_t);
    uint32_t riffSize = 4 + (8 + sizeof(fmt)) + (8 + extraPadded) + (8 + dataSize);

    writeChunkHeader(f, "RIFF", riffSize);
    f.write((const uint8_t *)"WAVE", 4);
    writeChunkHeader(f, "fmt ", sizeof(fmt));
    f.write((const uint8_t *)&fmt, sizeof(fmt));
    writeChunkHeader(f, "memo", extraSize);
    f.write((const uint8_t *)&extra, extraSize);
    if (extraPadded != extraSize) f.write((uint8_t)0);
    writeChunkHeader(f, "data", dataSize);
    size_t written = f.write((const uint8_t *)samples, dataSize);
    size_t fileSize = f.size();
    f.close();

    if (written != dataSize) {
        Serial.printf("Short write to %s (%u of %u bytes)\n", path, written, dataSize);
        SD_MMC.remove(path);
        memo.fileName[0] = 0;
        return false;
    }
    freeBytes -= min((uint64_t)fileSize, freeBytes);
    return true;
}

// Reads the header of one memo file. Accepts only what storageSaveMemo writes.
static bool readMemoHeader(File &f, Memo &memo) {
    char id[4];
    uint32_t size;
    if (f.read((uint8_t *)id, 4) != 4 || memcmp(id, "RIFF", 4) != 0) return false;
    f.read((uint8_t *)&size, 4);
    if (f.read((uint8_t *)id, 4) != 4 || memcmp(id, "WAVE", 4) != 0) return false;

    bool haveFormat = false, haveExtra = false;
    memo = {};
    strlcpy(memo.fileName, f.name(), sizeof(memo.fileName));
    while (f.read((uint8_t *)id, 4) == 4 && f.read((uint8_t *)&size, 4) == 4) {
        if (memcmp(id, "fmt ", 4) == 0 && size >= sizeof(WavFormat)) {
            WavFormat fmt;
            f.read((uint8_t *)&fmt, sizeof(fmt));
            haveFormat = fmt.audioFormat == 1 && fmt.channels == 1 &&
                         fmt.sampleRate == SAMPLE_RATE && fmt.bitsPerSample == 16;
            size -= sizeof(fmt);
        } else if (memcmp(id, "memo", 4) == 0 && size >= sizeof(MemoChunk)) {
            MemoChunk extra;
            f.read((uint8_t *)&extra, sizeof(extra));
            memo.number = extra.number;
            memo.recordedAt = extra.recordedAt;
            memo.gain = extra.gain;
            memcpy(memo.waveform, extra.waveform, WAVEFORM_BINS);
            haveExtra = true;
            size -= sizeof(extra);
        } else if (memcmp(id, "data", 4) == 0) {
            memo.dataOffset = f.position();
            memo.length = min(size, (uint32_t)(f.size() - memo.dataOffset)) / sizeof(int16_t);
            return haveFormat && haveExtra && memo.length > 0;
        }
        f.seek(f.position() + size + (size & 1));
    }
    return false;
}

int storageLoadMemos(Memo *out, int max) {
    if (!mounted) return 0;
    File dir = SD_MMC.open(MEMO_DIR);
    if (!dir) return 0;
    int count = 0;
    uint32_t start = millis();
    for (File f = dir.openNextFile(); f && count < max; f = dir.openNextFile()) {
        // f.name() is the bare file name in this core version
        const char *dot = strrchr(f.name(), '.');
        if (!f.isDirectory() && dot && strcasecmp(dot, ".wav") == 0) {
            if (readMemoHeader(f, out[count])) count++;
            else Serial.printf("Skipping %s: not a memo file\n", f.name());
        }
        f.close();
    }
    dir.close();
    Serial.printf("Loaded %d memos from SD card in %lu ms\n", count, millis() - start);
    return count;
}

bool storageDeleteMemo(const Memo &memo) {
    if (!mounted) return false;
    char path[48];
    memoPath(path, sizeof(path), memo.fileName);
    File f = SD_MMC.open(path);
    size_t fileSize = f ? f.size() : 0;
    f.close();
    if (!SD_MMC.remove(path)) return false;
    freeBytes += fileSize;
    return true;
}

File storageOpenMemo(const Memo &memo) {
    char path[48];
    memoPath(path, sizeof(path), memo.fileName);
    File f = SD_MMC.open(path);
    if (f) f.seek(memo.dataOffset);
    return f;
}
