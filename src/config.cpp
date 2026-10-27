#include "config.h"

#include <Arduino.h>
#include <Preferences.h>

const char *const ICON_NAMES[ICON_COUNT] = {"person", "tree", "smiley", "palette", "star",
                                            "wave",   "note", "heart",  "drop",    "sun"};

static const Category DEFAULT_CATEGORIES[CATEGORY_COUNT] = {
    {"Human", ICON_PERSON}, {"Nature", ICON_TREE}, {"Funny", ICON_SMILEY}, {"Art", ICON_PALETTE}, {"Other", ICON_STAR},
};

static SemaphoreHandle_t lock;
static Category categories[CATEGORY_COUNT];
static Schedule schedules[MAX_SCHEDULES]; // sorted by start
static int scheduleTotal = 0;
static volatile uint32_t version = 0;

static void saveCategories() {
    Preferences prefs;
    prefs.begin("zvukopis", false);
    prefs.putBytes("categories", categories, sizeof(categories));
    prefs.end();
}

static void saveSchedules() {
    Preferences prefs;
    prefs.begin("zvukopis", false);
    prefs.putBytes("schedules", schedules, scheduleTotal * sizeof(Schedule));
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
    memcpy(categories, DEFAULT_CATEGORIES, sizeof(categories));
    Preferences prefs;
    prefs.begin("zvukopis", true);
    if (prefs.getBytesLength("categories") == sizeof(categories)) prefs.getBytes("categories", categories, sizeof(categories));
    size_t len = prefs.getBytesLength("schedules");
    if (len % sizeof(Schedule) == 0 && len <= sizeof(schedules)) {
        scheduleTotal = prefs.getBytes("schedules", schedules, len) / sizeof(Schedule);
    }
    prefs.end();
    for (Category &c : categories) {
        c.name[CATEGORY_NAME_LEN - 1] = 0;
        if (c.icon >= ICON_COUNT) c.icon = ICON_STAR;
    }
    Serial.printf("[config] %d scheduled recording%s\n", scheduleTotal, scheduleTotal == 1 ? "" : "s");
}

uint32_t configVersion() { return version; }

Category configCategory(int slot) {
    Category c = {"", ICON_STAR};
    if (slot < 1 || slot > CATEGORY_COUNT) return c;
    xSemaphoreTake(lock, portMAX_DELAY);
    c = categories[slot - 1];
    xSemaphoreGive(lock);
    return c;
}

void configSetCategory(int slot, const Category &category) {
    if (slot < 1 || slot > CATEGORY_COUNT) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    Category &c = categories[slot - 1];
    strlcpy(c.name, category.name, sizeof(c.name));
    c.icon = category.icon < ICON_COUNT ? category.icon : ICON_STAR;
    saveCategories();
    version++;
    xSemaphoreGive(lock);
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
