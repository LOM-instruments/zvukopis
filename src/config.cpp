#include "config.h"

#include <Arduino.h>
#include <Preferences.h>

const char *const ICON_NAMES[ICON_COUNT] = {"person", "tree", "smiley", "palette", "star",
                                            "wave",   "note", "heart",  "drop",    "sun"};

static const Category DEFAULT_CATEGORIES[] = {
    {"Sound", ICON_WAVE}, {"Music", ICON_NOTE}, {"Note", ICON_STAR}, {"Biophony", ICON_TREE}, {"Anthropophony", ICON_PERSON},
};
static const int DEFAULT_CATEGORY_COUNT = sizeof(DEFAULT_CATEGORIES) / sizeof(DEFAULT_CATEGORIES[0]);

static SemaphoreHandle_t lock;
static Category categories[CATEGORY_MAX];
static int categoryTotal = DEFAULT_CATEGORY_COUNT;
static Schedule schedules[MAX_SCHEDULES]; // sorted by start
static int scheduleTotal = 0;
static volatile uint32_t version = 0;

static void saveCategories() {
    Preferences prefs;
    prefs.begin("zvukopis", false);
    // As many entries as are in use: the length of the blob is the count
    prefs.putBytes("categories", categories, categoryTotal * sizeof(Category));
    prefs.end();
}

static void saveSchedules() {
    Preferences prefs;
    prefs.begin("zvukopis", false);
    // Preferences ignores a zero-length write, which would bring removed entries back
    if (scheduleTotal > 0) prefs.putBytes("schedules", schedules, scheduleTotal * sizeof(Schedule));
    else prefs.remove("schedules");
    prefs.end();
}

static void sortSchedules() {
    qsort(schedules, scheduleTotal, sizeof(Schedule), [](const void *a, const void *b) {
        int64_t d = ((const Schedule *)a)->start - ((const Schedule *)b)->start;
        return d < 0 ? -1 : d > 0;
    });
}

void configInit() {
    lock = xSemaphoreCreateMutex();
    memcpy(categories, DEFAULT_CATEGORIES, sizeof(DEFAULT_CATEGORIES));
    Preferences prefs;
    prefs.begin("zvukopis", true);
    size_t stored = prefs.isKey("categories") ? prefs.getBytesLength("categories") : 0;
    if (stored >= sizeof(Category) && stored <= sizeof(categories) && stored % sizeof(Category) == 0) {
        categoryTotal = prefs.getBytes("categories", categories, stored) / sizeof(Category);
    }
    size_t len = prefs.isKey("schedules") ? prefs.getBytesLength("schedules") : 0;
    if (len % sizeof(Schedule) == 0 && len <= sizeof(schedules)) {
        scheduleTotal = prefs.getBytes("schedules", schedules, len) / sizeof(Schedule);
    }
    prefs.end();
    for (int i = 0; i < categoryTotal; i++) {
        Category &c = categories[i];
        c.name[CATEGORY_NAME_LEN - 1] = 0;
        if (c.icon >= ICON_COUNT) c.icon = ICON_STAR;
    }
    Serial.printf("[config] %d scheduled recording%s\n", scheduleTotal, scheduleTotal == 1 ? "" : "s");
}

uint32_t configVersion() { return version; }

int configCategoryCount() { return categoryTotal; }

Category configCategory(int slot) {
    Category c = {"", ICON_STAR};
    xSemaphoreTake(lock, portMAX_DELAY);
    if (slot >= 1 && slot <= categoryTotal) c = categories[slot - 1];
    xSemaphoreGive(lock);
    return c;
}

bool configSetCategories(const Category *list, int count) {
    if (count < 1 || count > CATEGORY_MAX) return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int i = 0; i < count; i++) {
        Category &c = categories[i];
        strlcpy(c.name, list[i].name, sizeof(c.name));
        c.icon = list[i].icon < ICON_COUNT ? list[i].icon : ICON_STAR;
    }
    categoryTotal = count;
    saveCategories();
    version++;
    xSemaphoreGive(lock);
    return true;
}

int scheduleCount() { return scheduleTotal; }

bool scheduleGet(int index, Schedule &out) {
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = index >= 0 && index < scheduleTotal;
    if (ok) out = schedules[index];
    xSemaphoreGive(lock);
    return ok;
}

bool scheduleAdd(const Schedule &schedule) {
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = scheduleTotal < MAX_SCHEDULES;
    if (ok) {
        schedules[scheduleTotal++] = schedule;
        sortSchedules();
        saveSchedules();
        version++;
    }
    xSemaphoreGive(lock);
    return ok;
}

void scheduleRemove(int index) {
    xSemaphoreTake(lock, portMAX_DELAY);
    if (index >= 0 && index < scheduleTotal) {
        memmove(&schedules[index], &schedules[index + 1], (scheduleTotal - index - 1) * sizeof(Schedule));
        scheduleTotal--;
        saveSchedules();
        version++;
    }
    xSemaphoreGive(lock);
}

bool scheduleNext(time_t now, Schedule &out, int &index) {
    xSemaphoreTake(lock, portMAX_DELAY);
    bool found = false;
    for (int i = 0; i < scheduleTotal; i++) {
        if (schedules[i].start + schedules[i].seconds > now) {
            out = schedules[i];
            index = i;
            found = true;
            break;
        }
    }
    xSemaphoreGive(lock);
    return found;
}

void scheduleDone(int64_t start, time_t now) {
    xSemaphoreTake(lock, portMAX_DELAY);
    int index = 0;
    while (index < scheduleTotal && schedules[index].start != start) index++;
    if (index < scheduleTotal) {
        Schedule &s = schedules[index];
        if (s.daily) {
            // Same local time on the next day, also across daylight saving changes
            while (s.start + s.seconds <= now) {
                time_t t = (time_t)s.start;
                tm local;
                localtime_r(&t, &local);
                local.tm_mday++;
                local.tm_isdst = -1;
                s.start = mktime(&local);
            }
            sortSchedules();
        } else {
            memmove(&schedules[index], &schedules[index + 1], (scheduleTotal - index - 1) * sizeof(Schedule));
            scheduleTotal--;
        }
        saveSchedules();
        version++;
    }
    xSemaphoreGive(lock);
}
