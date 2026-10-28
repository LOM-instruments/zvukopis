#pragma once

#include <stddef.h>
#include <stdint.h>

// Documents to read on the device: Markdown (or plain text) files in /doc on
// the SD card, put there from the web interface or a computer. The reader in
// ui.cpp lays them out; this keeps the list, loads a document into PSRAM as
// ASCII, and remembers the page each one was left at.

static const int MAX_DOCUMENTS = 64;
static const size_t MAX_DOCUMENT_BYTES = 512 * 1024;

struct Document {
    char fileName[48];
    char title[48]; // the first heading, or the file name
    uint32_t bytes;
};

void libraryScan(); // reads /doc (call after the card is mounted, and after uploads)
int libraryCount();
bool libraryGet(int index, Document &out); // by title

// The document's text, as ASCII with typographic punctuation mapped
// (curly quotes, dashes, ellipsis), in PSRAM until the next load. nullptr if
// it can't be read.
char *libraryLoad(int index, size_t &length);

// Indices change when the list is rescanned (an upload from the web), so
// these go by file name
int libraryFind(const char *fileName); // -1 if gone
int libraryLastPage(const char *fileName);
void librarySetLastPage(const char *fileName, int page);
bool libraryDelete(const char *fileName); // forgets its page too
const char *libraryDir();
