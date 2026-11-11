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
static const uint32_t MAX_SCHEDULE_SECONDS = 24 * 3600;

struct Schedule {
    int64_t start;    // UTC
    uint32_t seconds; // how long to record
    uint8_t daily;    // repeats every day at the same time
    // A daily entry set to a time in the hour the clocks skip in spring runs
    // later on that one day: by this many minutes, taken off again for the
    // day after. Kept by scheduleDone(); scheduleAdd() clears it.
    int16_t movedMinutes;
};
// The list is saved as it lies in memory. movedMinutes sits where the struct
// had padding before, so a list saved by an earlier version still loads.
static_assert(sizeof(Schedule) == 16, "the saved schedule list depends on this layout");

int scheduleCount();
bool scheduleGet(int index, Schedule &out); // in start order
// False when full, for a start outside the clock's range (clock.h), and for a
// length outside 1 s to MAX_SCHEDULE_SECONDS
bool scheduleAdd(const Schedule &schedule);
void scheduleRemove(int index);
// The earliest entry not yet over at `now` (it may already have started).
// False when there's none.
bool scheduleNext(time_t now, Schedule &out, int &index);
// Called once the recording of the entry starting at `start` has run (or was
// missed): removes it, or for a daily one moves it to its next day, also when
// the recording was stopped before its time was up
void scheduleDone(int64_t start, time_t now);
