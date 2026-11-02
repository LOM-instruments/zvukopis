#pragma once

#include <stdint.h>
#include <time.h>

// Settings made on the web interface, kept in flash (NVS): the categories a
// moment can be tagged with, and scheduled recordings. Safe to use from any task.

// --- Categories ---

// Tag slots 1..CATEGORY_MAX; a moment stores its slot. The first
// configCategoryCount() slots are in use; the rest return an empty name.
static const int CATEGORY_MAX = 12;
static const int CATEGORY_NAME_LEN = 16;

enum CategoryIcon : uint8_t {
    ICON_PERSON,
    ICON_TREE,
    ICON_SMILEY,
    ICON_PALETTE,
    ICON_STAR,
    ICON_WAVE,
    ICON_NOTE,
    ICON_HEART,
    ICON_DROP,
    ICON_SUN,
    ICON_COUNT,
};

extern const char *const ICON_NAMES[ICON_COUNT]; // for the web interface

struct Category {
    char name[CATEGORY_NAME_LEN];
    uint8_t icon; // CategoryIcon
};

void configInit();
int configCategoryCount();         // 1..CATEGORY_MAX
Category configCategory(int slot); // slot 1..configCategoryCount(); an empty name beyond
bool configSetCategories(const Category *list, int count); // the whole set at once (1..CATEGORY_MAX)
uint32_t configVersion(); // changes whenever categories or the schedule do

// --- Scheduled recordings ---

static const int MAX_SCHEDULES = 16;

struct Schedule {
    int64_t start;    // UTC
    uint32_t seconds; // how long to record
    uint8_t daily;    // repeats every day at the same time
};

int scheduleCount();
bool scheduleGet(int index, Schedule &out); // in start order
bool scheduleAdd(const Schedule &schedule); // false when full
void scheduleRemove(int index);
// The earliest entry not yet over at `now` (it may already have started).
// False when there's none.
bool scheduleNext(time_t now, Schedule &out, int &index);
// Called once the recording of the entry starting at `start` has run (or was
// missed): removes it, or for a daily one moves it to its next day
void scheduleDone(int64_t start, time_t now);
