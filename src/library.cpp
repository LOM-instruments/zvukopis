#include "library.h"

#include <Arduino.h>
#include <Preferences.h>
#include <SD_MMC.h>

#include "guide_text.h" // GUIDE_TEXT, GUIDE_LENGTH: docs/guide.md as built in by scripts/embed_guide.py
#include "storage.h"

static const char *DOC_DIR = "/doc";
// The user guide's name among the documents. A guide under another name
// (the website serves it as guide.md) is the user's own and is left alone.
static const char *GUIDE_FILE = "zvukopis-guide.md";

static Document *documents = nullptr; // MAX_DOCUMENTS, in PSRAM
static int documentTotal = 0;
static char *text = nullptr; // the loaded document
static SemaphoreHandle_t lock;

const char *libraryDir() { return DOC_DIR; }

static void docPath(char *buf, size_t len, const char *fileName) {
    snprintf(buf, len, "%s/%s", DOC_DIR, fileName);
}

// The first "# heading" line, if there is one near the top
static void readTitle(File &f, Document &doc) {
    char line[96];
    for (int i = 0; i < 20 && f.available(); i++) {
        size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[n] = 0;
        char *p = line;
        while (*p == ' ' || *p == '\r') p++;
        if (*p == '#') {
            while (*p == '#') p++;
            while (*p == ' ') p++;
            char *end = p + strlen(p);
            while (end > p && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '#')) *--end = 0;
            if (*p) {
                strlcpy(doc.title, p, sizeof(doc.title));
                return;
            }
        }
    }
    strlcpy(doc.title, doc.fileName, sizeof(doc.title));
    char *dot = strrchr(doc.title, '.');
    if (dot && dot != doc.title) *dot = 0;
}

bool libraryIsGuide(const char *fileName) {
    return fileName && strcasecmp(fileName, GUIDE_FILE) == 0; // FAT names don't tell upper case from lower
}

// True when the file holds this firmware's guide, byte for byte
static bool holdsTheGuide(const char *path) {
    if (!SD_MMC.exists(path)) return false;
    File f = SD_MMC.open(path);
    if (!f) return false;
    bool same = !f.isDirectory() && f.size() == GUIDE_LENGTH;
    uint8_t block[256];
    for (size_t at = 0; same && at < GUIDE_LENGTH;) {
        size_t n = f.read(block, min(sizeof(block), GUIDE_LENGTH - at));
        same = n > 0 && memcmp(block, GUIDE_TEXT + at, n) == 0;
        at += n;
    }
    f.close();
    return same;
}

void libraryInstallGuide() {
    if (!storageAvailable()) return;
    uint32_t t0 = millis();
    char path[64], part[64];
    docPath(path, sizeof(path), GUIDE_FILE);
    docPath(part, sizeof(part), "guide.part"); // a name the library does not list
    if (!holdsTheGuide(path)) {
        // Written beside it first and read back, and only then put in its
        // place: a full card or a power cut leaves the guide that was there
        if (!SD_MMC.exists(DOC_DIR)) SD_MMC.mkdir(DOC_DIR);
        File f = SD_MMC.open(part, FILE_WRITE);
        if (f) {
            f.write(GUIDE_TEXT, GUIDE_LENGTH);
            f.close();
        }
        bool ok = holdsTheGuide(part);
        if (ok && SD_MMC.exists(path)) ok = SD_MMC.remove(path);
        ok = ok && SD_MMC.rename(part, path);
        if (!ok) {
            SD_MMC.remove(part);
            Serial.println("[library] the user guide could not be written to the card");
            return;
        }
        Serial.printf("[library] user guide written to %s (%u bytes, %lu ms)\n", path, (unsigned)GUIDE_LENGTH, millis() - t0);
    } else {
        Serial.printf("[library] the card has this firmware's user guide (checked in %lu ms)\n", millis() - t0);
    }
}

void libraryScan() {
    if (!lock) lock = xSemaphoreCreateMutex();
    if (!documents) documents = (Document *)ps_calloc(MAX_DOCUMENTS, sizeof(Document));
    if (!documents) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    documentTotal = 0;
    if (storageAvailable()) {
        File dir = SD_MMC.open(DOC_DIR);
        for (File f = dir ? dir.openNextFile() : File(); f && documentTotal < MAX_DOCUMENTS; f = dir.openNextFile()) {
            const char *dot = strrchr(f.name(), '.');
            bool doc = !f.isDirectory() && dot && (strcasecmp(dot, ".md") == 0 || strcasecmp(dot, ".txt") == 0) &&
                       f.name()[0] != '.' && strlen(f.name()) < sizeof(documents->fileName);
            if (doc && f.size() > 0 && f.size() <= MAX_DOCUMENT_BYTES) {
                Document &d = documents[documentTotal++];
                strlcpy(d.fileName, f.name(), sizeof(d.fileName));
                d.bytes = f.size();
                readTitle(f, d);
            }
            f.close();
        }
        if (dir) dir.close();
        qsort(documents, documentTotal, sizeof(Document), [](const void *a, const void *b) {
            return strcasecmp(((const Document *)a)->title, ((const Document *)b)->title);
        });
    }
    xSemaphoreGive(lock);
    Serial.printf("[library] %d document%s in %s\n", documentTotal, documentTotal == 1 ? "" : "s", DOC_DIR);
}

int libraryCount() { return documentTotal; }

bool libraryGet(int index, Document &out) {
    if (!lock) return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = index >= 0 && index < documentTotal;
    if (ok) out = documents[index];
    xSemaphoreGive(lock);
    return ok;
}

// UTF-8 to the ASCII the fonts have: typographic punctuation gets its plain
// form, anything else becomes '?'
static size_t toAscii(char *s, size_t n) {
    size_t w = 0;
    for (size_t r = 0; r < n;) {
        uint8_t c = s[r];
        if (c < 0x80) {
            if (c != '\r') s[w++] = c;
            r++;
            continue;
        }
        int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
        uint32_t cp = c & (0x7F >> len);
        for (int i = 1; i < len && r + i < n; i++) cp = cp << 6 | (s[r + i] & 0x3F);
        r += len;
        const char *out = "?";
        switch (cp) {
        case 0x2018: case 0x2019: case 0x201A: case 0x2032: case 0x02BC: out = "'"; break;
        case 0x201C: case 0x201D: case 0x201E: case 0x2033: out = "\""; break;
        case 0x2013: out = "-"; break;
        case 0x2014: case 0x2015: out = "--"; break;
        case 0x2026: out = "..."; break;
        case 0x00A0: case 0x2009: case 0x202F: out = " "; break;
        case 0x00AB: case 0x00BB: out = "\""; break;
        case 0x00E9: case 0x00E8: case 0x00EA: out = "e"; break;
        case 0x00E1: case 0x00E0: case 0x00E2: case 0x00E4: out = "a"; break;
        case 0x00F6: case 0x00F3: case 0x00F4: out = "o"; break;
        case 0x00FC: case 0x00FA: out = "u"; break;
        case 0x00ED: case 0x00EE: out = "i"; break;
        case 0x00E7: out = "c"; break;
        case 0x00DF: out = "ss"; break;
        }
        for (const char *p = out; *p; p++) s[w++] = *p; // never longer than the UTF-8 it replaces
    }
    return w;
}

char *libraryLoad(int index, size_t &length) {
    Document doc;
    if (!libraryGet(index, doc)) return nullptr;
    char path[64];
    docPath(path, sizeof(path), doc.fileName);
    File f = SD_MMC.open(path);
    if (!f) return nullptr;
    free(text);
    text = (char *)ps_malloc(doc.bytes + 1);
    if (!text) {
        f.close();
        return nullptr;
    }
    size_t n = f.read((uint8_t *)text, doc.bytes);
    f.close();
    n = toAscii(text, n);
    text[n] = 0;
    length = n;
    return text;
}

void libraryUnload() {
    free(text);
    text = nullptr;
}

int libraryFind(const char *fileName) {
    if (!lock || !fileName) return -1;
    xSemaphoreTake(lock, portMAX_DELAY);
    int found = -1;
    for (int i = 0; i < documentTotal && found < 0; i++) {
        if (strcmp(documents[i].fileName, fileName) == 0) found = i;
    }
    xSemaphoreGive(lock);
    return found;
}

// NVS keys are short: "p" + a hash of the file name
static void pageKey(const char *fileName, char *key, size_t len) {
    uint32_t h = 2166136261u;
    for (const char *p = fileName; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    snprintf(key, len, "p%08lx", (unsigned long)h);
}

int libraryLastPage(const char *fileName) {
    char key[12];
    pageKey(fileName, key, sizeof(key));
    Preferences prefs;
    prefs.begin("library", true);
    int page = prefs.getInt(key, 0);
    prefs.end();
    return page;
}

void librarySetLastPage(const char *fileName, int page) {
    char key[12];
    pageKey(fileName, key, sizeof(key));
    Preferences prefs;
    prefs.begin("library", false);
    if (page <= 0) { if (prefs.isKey(key)) prefs.remove(key); }
    else if (prefs.getInt(key, -1) != page) prefs.putInt(key, page);
    prefs.end();
}

bool libraryDelete(const char *fileName) {
    if (libraryIsGuide(fileName)) return false; // it would be back at the next start
    if (libraryFind(fileName) < 0) return false;
    char path[64];
    docPath(path, sizeof(path), fileName);
    bool ok = SD_MMC.remove(path);
    librarySetLastPage(fileName, 0);
    libraryScan();
    return ok;
}
