#include "ui.h"

#include <Adafruit_GFX.h>
#include <Arduino.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSerifItalic12pt7b.h>
#include <Fonts/FreeSerifItalic18pt7b.h>
#include <Fonts/FreeSerifItalic9pt7b.h>
#include <mbedtls/base64.h>
#include <sys/time.h>

#include "EPD_3in97.h"
#include "audio.h"
#include "board_power.h"
#include "cardreader.h"
#include "clock.h"
#include "quotes.h"
#include "storage.h"
#include "transfer.h"

// Canvas bits match the panel: 1 = white, 0 = black
static const uint16_t INK = 0;
static const uint16_t PAPER = 1;

// Portrait: the canvas keeps the panel's 800x480 memory layout and rotates drawing.
// Use 3 instead of 1 to turn the screen upside down.
static const int SCREEN_ROTATION = 3;
static const int SW = EPD_3IN97_HEIGHT; // 480
static const int SH = EPD_3IN97_WIDTH;  // 800

static const int MARGIN = 24;
static const int HEADER_LINE_Y = 100;
static const int LIST_TOP = 116;
static const int ROW_H = 96;
static const int VISIBLE_ROWS = 6;
static const int FOOTER_LINE_Y = 708;
static const int WAVE_X = MARGIN + 44;
static const int WAVE_PITCH = 5; // bar width 3 + gap 2
static const int WAVE_HALF_H = 15;
static const int ICON_BOX = 40; // header buttons

static const int FULL_REFRESH_EVERY = 30; // partial refreshes, before a full one clears ghosting
static const uint32_t FULL_REFRESH_IDLE_MS = 20000; // ...done once the buttons have been left alone this long
static const uint32_t CONFIRM_TIMEOUT_MS = 10000;
static const uint32_t MENU_TIMEOUT_MS = 15000;
static const uint32_t SLEEP_SCREEN_TIMEOUT_MS = 15000;
static const uint32_t BATTERY_READ_MS = 30000;
static const uint32_t SETTINGS_READ_MS = 2000;       // USB plugged in or not, while settings are up
static const uint32_t AUTO_SLEEP_MS = 5 * 60 * 1000; // idle time before sleeping on its own
static const uint32_t RESULT_MS = 10000;             // how long a sync or format result stays up

enum Screen {
    SCREEN_LIST,     // all moments, newest first
    SCREEN_CALENDAR, // month grid; moves between days that have moments
    SCREEN_DAY,      // the moments of one day
    SCREEN_SETTINGS,
    SCREEN_TRANSFER,    // Wi-Fi file transfer running
    SCREEN_CARD_READER, // SD card open over USB
};

enum Overlay {
    OVERLAY_NONE,
    OVERLAY_MENU,           // tag / delete, on a long press of a moment
    OVERLAY_CONFIRM_DELETE, // Cancel / Delete
    OVERLAY_CONFIRM_FORMAT, // Cancel / Format
    OVERLAY_GAIN,           // mic gain choices
};

// Header buttons have negative positions: up from the first row reaches them
static const int ROW_BACK = -1;     // sub-screens
static const int ROW_CALENDAR = -1; // main list
static const int ROW_SETTINGS = -2; // main list

enum SettingsRow { SET_SYNC, SET_GAIN, SET_TRANSFER, SET_CARD_READER, SET_FORMAT, SET_ROWS };
static const int GAIN_CHOICES[] = {GAIN_AUTO, 0, 6, 12, 18, 24, 30, 36, 42, 48, 54};
static const int GAIN_CHOICE_COUNT = sizeof(GAIN_CHOICES) / sizeof(GAIN_CHOICES[0]);
static const int MENU_DELETE = TAG_COUNT - 1; // after the five tags
static const int MENU_CANCEL = TAG_COUNT;
static const int MENU_ITEMS = TAG_COUNT + 1;

static const char *TAG_LABELS[TAG_COUNT] = {"", "Human", "Nature", "Funny", "Art", "Other"};

// UI state: written by loop(), read by the display task
static volatile int screen = SCREEN_LIST;
static volatile int selected = 0;    // list: row 0 = New recording, row n = moment n-1; day: row n = its moment n
static volatile int calendarDay = 0; // yyyymmdd of the day chosen in the calendar
static volatile int overlay = OVERLAY_NONE;
static volatile int overlayChoice = 0;
static uint32_t overlaySince = 0;
static int menuMoment = 0; // number of the moment the menu is for
static volatile bool sleepRequested = false;
static volatile uint32_t lastActivity = 0; // last button press, or recording/playback still running
static volatile bool sleeping = false;    // what the screen should show
static volatile bool panelAsleep = false; // what the display task has done
static volatile int quoteIndex = 0;
static volatile uint32_t quietSince = 0; // display task found nothing to redraw from then on; 0 while busy
static int lastMomentCount = 0;
static int lastFormatResult = 0;
static uint32_t formatResultAt = 0;
static volatile bool screenshotRequested = false;
static TaskHandle_t displayTaskHandle = nullptr; // woken on input, so it needn't wait for its next poll

static GFXcanvas1 *canvas;

// --- Days with moments, for the calendar ---

struct DayInfo {
    int key;     // yyyymmdd, local time
    int count;   // moments that day
    int seconds; // their total length
    uint8_t tags; // bit per MomentTag present
};

// Each task keeps its own copy, rebuilt when the moment list changes
struct DayIndex {
    uint32_t version = UINT32_MAX;
    int count = 0;
    DayInfo *days = nullptr; // oldest first
};

static DayIndex loopDays, displayDays;

static int dayKey(time_t t) {
    tm local;
    localtime_r(&t, &local);
    return (local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday;
}

static int momentSeconds(const Moment &moment) {
    return max(1, (int)((moment.length + SAMPLE_RATE / 2) / SAMPLE_RATE));
}

static const DayIndex &daysWithMoments(DayIndex &index) {
    uint32_t version = momentListVersion();
    if (index.version == version) return index;
    if (!index.days) index.days = (DayInfo *)ps_malloc(MAX_MOMENTS * sizeof(DayInfo));
    index.count = 0;
    Moment moment;
    for (int i = 0; momentGet(i, moment); i++) {
        if (!moment.recordedAt) continue;
        int key = dayKey(moment.recordedAt);
        int d = 0;
        while (d < index.count && index.days[d].key != key) d++;
        if (d == index.count) index.days[index.count++] = {key, 0, 0, 0};
        index.days[d].count++;
        index.days[d].seconds += momentSeconds(moment);
        index.days[d].tags |= 1 << moment.tag;
    }
    qsort(index.days, index.count, sizeof(DayInfo), [](const void *a, const void *b) {
        return ((const DayInfo *)a)->key - ((const DayInfo *)b)->key;
    });
    index.version = version;
    return index;
}

static int findDay(const DayIndex &index, int key) {
    for (int d = 0; d < index.count; d++) {
        if (index.days[d].key == key) return d;
    }
    return -1;
}

// List positions of the moments recorded on `key`, newest first
static int momentsOnDay(int key, int *out, int max) {
    int n = 0;
    Moment moment;
    for (int i = 0; n < max && momentGet(i, moment); i++) {
        if (moment.recordedAt && dayKey(moment.recordedAt) == key) out[n++] = i;
    }
    return n;
}

static int *loopDayMoments, *displayDayMoments; // MAX_MOMENTS each

static int momentIndexByNumber(int number) {
    Moment moment;
    for (int i = 0; momentGet(i, moment); i++) {
        if (moment.number == number) return i;
    }
    return -1;
}

// Everything the screen depends on; the display task redraws when it changes
struct ViewState {
    int screen;
    int selected;
    int calendarDay;
    int overlay;
    int overlayChoice;
    int menuMoment;
    int sleeping;
    int quote;
    int momentCount;
    uint32_t listVersion;
    int audio;
    int playingIndex;
    int playSeconds;
    int speedTenths; // playback speed x10
    int secondsLeft;
    int battery;
    int usb;
    int sdCard;
    int clockMinute; // -1 while the clock isn't set; changes once a minute
    int clockSync;   // ClockSync, NONE once a result has been up for RESULT_MS
    int formatStatus; // 0, 1 done, -1 failed (for RESULT_MS), 2 formatting
    int transferState;
    uint32_t transferRequests;
    int cardEjected;
    int gainSetting;
    int gainNow;
};

static ViewState currentView() {
    static int battery = -1, usb = 0;
    static uint32_t batteryReadAt = 0;
    uint32_t readEvery = screen == SCREEN_SETTINGS ? SETTINGS_READ_MS : BATTERY_READ_MS;
    if (batteryReadAt == 0 || millis() - batteryReadAt > readEvery) {
        battery = batteryPercent();
        usb = usbPowered();
        batteryReadAt = millis();
    }

    ViewState v;
    memset(&v, 0, sizeof(v)); // compared with memcmp
    v.screen = screen;
    v.selected = selected;
    v.calendarDay = calendarDay;
    v.overlay = overlay;
    v.overlayChoice = overlayChoice;
    v.menuMoment = menuMoment;
    v.sleeping = sleeping;
    v.quote = quoteIndex;
    v.momentCount = momentCount();
    v.listVersion = momentListVersion();
    v.audio = audioState();
    v.playingIndex = audioPlayingIndex();
    v.playSeconds = (int)audioPlaybackSeconds();
    v.speedTenths = (int)(audioPlaybackSpeed() * 10 + 0.5f);
    v.secondsLeft = (int)audioSecondsLeft();
    v.battery = battery;
    v.usb = usb;
    v.sdCard = audioUsingSdCard();
    v.clockMinute = clockValid() ? (int)(time(nullptr) / 60) : -1;
    uint32_t since;
    ClockSync sync = clockSyncState(since);
    if (sync != CLOCK_SYNCING && millis() - since > RESULT_MS) sync = CLOCK_SYNC_NONE;
    v.clockSync = sync;
    if (v.audio == AUDIO_FORMATTING) v.formatStatus = 2;
    else if (lastFormatResult != 0 && millis() - formatResultAt < RESULT_MS) v.formatStatus = lastFormatResult;
    TransferInfo transfer = transferInfo();
    v.transferState = transfer.state;
    v.transferRequests = transfer.requests;
    v.cardEjected = screen == SCREEN_CARD_READER && cardReaderEjected();
    v.gainSetting = audioMicGainSetting();
    v.gainNow = audioMicGain();
    return v;
}

// --- Drawing helpers ---

static void formatTime(char *buf, size_t len, int seconds) {
    snprintf(buf, len, "%d:%02d", seconds / 60, seconds % 60);
}

static void formatTimeLeft(char *buf, size_t len, int seconds) {
    if (seconds >= 3600) snprintf(buf, len, "%dh %02dm", seconds / 3600, seconds / 60 % 60);
    else formatTime(buf, len, seconds);
}

// strftime of a UTC time in local time, without the leading space of %e
static void formatLocal(char *buf, size_t len, const char *format, time_t t) {
    tm local;
    localtime_r(&t, &local);
    strftime(buf, len, format, &local);
    if (buf[0] == ' ') memmove(buf, buf + 1, strlen(buf));
}

static void formatBytes(char *buf, size_t len, uint64_t bytes) {
    if (bytes >= (1ULL << 30)) snprintf(buf, len, "%.1f GB", bytes / 1073741824.0);
    else snprintf(buf, len, "%llu MB", bytes >> 20);
}

static int textWidth(const GFXfont *font, const char *text) {
    int16_t x1, y1;
    uint16_t w, h;
    canvas->setFont(font);
    canvas->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    return x1 + w;
}

// y is the text baseline
static void drawText(int x, int y, const GFXfont *font, const char *text, uint16_t color) {
    canvas->setFont(font);
    canvas->setTextColor(color);
    canvas->setCursor(x, y);
    canvas->print(text);
}

static void drawTextRight(int right, int y, const GFXfont *font, const char *text, uint16_t color) {
    drawText(right - textWidth(font, text), y, font, text, color);
}

static void drawTextCentered(int cx, int y, const GFXfont *font, const char *text, uint16_t color) {
    drawText(cx - textWidth(font, text) / 2, y, font, text, color);
}

// Word-wrapped and centred; returns the baseline after the last line.
// With draw false it only measures.
static int drawWrapped(int cx, int y, int maxWidth, int lineHeight, const GFXfont *font, const char *text,
                       bool draw = true) {
    char line[96];
    const char *p = text;
    while (*p) {
        // Take as many words as fit
        int len = 0, fit = 0;
        while (p[len]) {
            int end = len;
            while (p[end] && p[end] != ' ') end++;
            if (end >= (int)sizeof(line)) break;
            memcpy(line, p, end);
            line[end] = 0;
            if (fit > 0 && textWidth(font, line) > maxWidth) break;
            fit = end;
            len = p[end] ? end + 1 : end;
        }
        if (fit == 0) fit = len = strlen(p) < sizeof(line) ? strlen(p) : sizeof(line) - 1;
        memcpy(line, p, fit);
        line[fit] = 0;
        if (draw) drawTextCentered(cx, y, font, line, INK);
        y += lineHeight;
        p += len;
    }
    return y;
}

static void dottedLine(int x0, int x1, int y) {
    for (int x = x0; x <= x1; x += 4) canvas->drawPixel(x, y, INK);
}

// Arc of a thick circle outline, angles in degrees clockwise from 3 o'clock
static void drawArc(int cx, int cy, int r, int fromDeg, int toDeg, int thickness, uint16_t c) {
    for (int t = 0; t < thickness; t++) {
        float rr = r - t;
        int steps = max(8, (int)(rr * (toDeg - fromDeg) / 40));
        for (int i = 0; i <= steps; i++) {
            float a = (fromDeg + (toDeg - fromDeg) * (float)i / steps) * (float)M_PI / 180;
            canvas->drawPixel(cx + (int)roundf(rr * cosf(a)), cy + (int)roundf(rr * sinf(a)), c);
        }
    }
}

static void iconPlay(int cx, int cy, uint16_t c) {
    canvas->fillTriangle(cx - 6, cy - 9, cx - 6, cy + 9, cx + 9, cy, c);
}

static void iconStop(int cx, int cy, uint16_t c) {
    canvas->fillRect(cx - 7, cy - 7, 15, 15, c);
}

static void iconRecord(int cx, int cy, uint16_t c) {
    canvas->drawCircle(cx, cy, 11, c);
    canvas->drawCircle(cx, cy, 10, c);
    canvas->fillCircle(cx, cy, 6, c);
}

static void iconUpDown(int cx, int cy, uint16_t c) {
    canvas->fillTriangle(cx - 6, cy - 3, cx + 6, cy - 3, cx, cy - 10, c);
    canvas->fillTriangle(cx - 6, cy + 3, cx + 6, cy + 3, cx, cy + 10, c);
}

static void iconClick(int cx, int cy, bool hold) {
    canvas->fillCircle(cx, cy, 6, INK);
    if (hold) canvas->drawCircle(cx, cy, 10, INK);
}

static void iconBolt(int cx, int cy) {
    canvas->fillTriangle(cx + 2, cy - 10, cx - 5, cy + 1, cx + 1, cy + 1, INK);
    canvas->fillTriangle(cx - 1, cy - 1, cx + 5, cy - 1, cx - 2, cy + 10, INK);
}

// Returns the icon's left edge
static int iconSdCard(int right, int cy, int scale = 1) {
    const int w = 16 * scale, h = 20 * scale;
    int x = right - w, y = cy - h / 2;
    canvas->fillRect(x, y, w, h, INK);
    canvas->fillTriangle(x + w - 6 * scale, y, x + w, y, x + w, y + 6 * scale, PAPER); // clipped corner
    for (int i = 0; i < 3; i++) canvas->fillRect(x + (3 + 3 * i) * scale, y + 3 * scale, 2 * scale, 5 * scale, PAPER);
    return x;
}

static void iconStar(int cx, int cy, int r, uint16_t c = INK) {
    canvas->fillTriangle(cx - r, cy, cx + r, cy, cx, cy - r / 3, c);
    canvas->fillTriangle(cx - r, cy, cx + r, cy, cx, cy + r / 3, c);
    canvas->fillTriangle(cx, cy - r, cx, cy + r, cx - r / 3, cy, c);
    canvas->fillTriangle(cx, cy - r, cx, cy + r, cx + r / 3, cy, c);
}

static void iconTrash(int cx, int cy) {
    canvas->fillRoundRect(cx - 28, cy - 34, 56, 6, 2, INK); // lid
    canvas->fillRect(cx - 9, cy - 40, 18, 6, INK);           // handle
    for (int i = 0; i < 3; i++) canvas->drawRect(cx - 22 + i, cy - 22 + i, 44 - 2 * i, 56 - 2 * i, INK);
    for (int dx = -10; dx <= 10; dx += 10) canvas->fillRect(cx + dx - 1, cy - 12, 3, 36, INK);
}

static void iconGear(int cx, int cy, uint16_t fg, uint16_t bg) {
    for (int i = 0; i < 8; i++) {
        float a = i * (float)M_PI / 4;
        canvas->fillCircle(cx + (int)roundf(10 * cosf(a)), cy + (int)roundf(10 * sinf(a)), 3, fg);
    }
    canvas->fillCircle(cx, cy, 9, fg);
    canvas->fillCircle(cx, cy, 4, bg);
}

static void iconCalendar(int cx, int cy, uint16_t fg, uint16_t bg) {
    int x = cx - 12, y = cy - 10;
    canvas->fillRoundRect(x, y, 24, 22, 3, fg);
    canvas->fillRect(x + 2, y + 7, 20, 13, bg);
    canvas->fillRect(x + 5, y - 3, 3, 6, fg); // rings
    canvas->fillRect(x + 16, y - 3, 3, 6, fg);
    for (int r = 0; r < 2; r++) {
        for (int c = 0; c < 3; c++) canvas->fillRect(x + 5 + c * 6, y + 10 + r * 5, 3, 3, fg);
    }
}

static void iconWifi(int cx, int cy) {
    for (int r = 14; r <= 42; r += 14) drawArc(cx, cy, r, 225, 315, 5, INK);
    canvas->fillCircle(cx, cy - 2, 5, INK);
}

// What a moment is about; r is about half the icon's size
static void iconTag(int tag, int cx, int cy, int r, uint16_t c) {
    switch (tag) {
    case TAG_HUMAN: // head and shoulders
        canvas->fillCircle(cx, cy - r * 4 / 10, r * 4 / 10, c);
        canvas->fillRoundRect(cx - r * 8 / 10, cy + r / 10, r * 16 / 10, r * 9 / 10, r * 4 / 10, c);
        break;
    case TAG_NATURE: // a tree
        canvas->fillTriangle(cx, cy - r, cx - r * 8 / 10, cy + r * 4 / 10, cx + r * 8 / 10, cy + r * 4 / 10, c);
        canvas->fillRect(cx - max(1, r / 6), cy + r * 4 / 10, max(2, r / 3), r * 6 / 10, c);
        break;
    case TAG_FUNNY: // a smiley
        drawArc(cx, cy, r, 0, 360, max(2, r / 6), c);
        canvas->fillCircle(cx - r * 35 / 100, cy - r * 3 / 10, max(1, r / 7), c);
        canvas->fillCircle(cx + r * 35 / 100, cy - r * 3 / 10, max(1, r / 7), c);
        drawArc(cx, cy, r * 55 / 100, 25, 155, max(2, r / 7), c);
        break;
    case TAG_ART: { // a palette
        uint16_t hole = c == INK ? PAPER : INK;
        canvas->fillCircle(cx, cy, r, c);
        canvas->fillCircle(cx + r * 4 / 10, cy + r * 4 / 10, r * 25 / 100, hole);
        canvas->fillCircle(cx - r * 45 / 100, cy - r * 15 / 100, max(1, r / 6), hole);
        canvas->fillCircle(cx - r * 10 / 100, cy - r * 50 / 100, max(1, r / 6), hole);
        canvas->fillCircle(cx + r * 35 / 100, cy - r * 35 / 100, max(1, r / 6), hole);
        break;
    }
    case TAG_OTHER:
        iconStar(cx, cy, r, c);
        break;
    }
}

// Returns the battery's left edge
static int iconBattery(int right, int cy, int percent) {
    const int bodyW = 34, bodyH = 17, nubW = 3;
    int x = right - nubW - bodyW;
    int y = cy - bodyH / 2;
    canvas->drawRect(x, y, bodyW, bodyH, INK);
    canvas->drawRect(x + 1, y + 1, bodyW - 2, bodyH - 2, INK);
    canvas->fillRect(x + bodyW, cy - 4, nubW, 9, INK);
    int fill = (bodyW - 8) * constrain(percent, 0, 100) / 100;
    canvas->fillRect(x + 4, y + 4, fill, bodyH - 8, INK);
    return x;
}

enum HintIcon { HINT_UPDOWN, HINT_CLICK, HINT_HOLD_CLICK, HINT_PWR };

// A key name in a rounded box, like a keycap. Returns its width.
static int drawKeycap(int x, int cy, const char *name) {
    int w = textWidth(&FreeSans9pt7b, name) + 12;
    canvas->drawRoundRect(x, cy - 12, w, 24, 5, INK);
    drawText(x + 6, cy + 6, &FreeSans9pt7b, name, INK);
    return w;
}

static void drawHint(int x, int cy, HintIcon icon, const char *label) {
    switch (icon) {
    case HINT_UPDOWN:
        iconUpDown(x + 6, cy, INK);
        x += 20;
        break;
    case HINT_CLICK:
        iconClick(x + 10, cy, false);
        x += 26;
        break;
    case HINT_HOLD_CLICK:
        iconClick(x + 10, cy, true);
        x += 28;
        break;
    case HINT_PWR:
        x += drawKeycap(x, cy, "PWR") + 8;
        break;
    }
    drawText(x, cy + 7, &FreeSans12pt7b, label, INK);
}

// A header button: icon in a rounded box, filled when selected
static void drawHeaderButton(int x, int cy, bool isSelected, void (*icon)(int, int, uint16_t, uint16_t)) {
    int y = cy - ICON_BOX / 2;
    uint16_t fg = isSelected ? PAPER : INK, bg = isSelected ? INK : PAPER;
    if (isSelected) canvas->fillRoundRect(x, y, ICON_BOX, ICON_BOX, 9, INK);
    else canvas->drawRoundRect(x, y, ICON_BOX, ICON_BOX, 9, INK);
    icon(x + ICON_BOX / 2, cy, fg, bg);
}

// --- Screen sections ---

// SD card, USB and battery, right-aligned at `right`; returns the left edge
static int drawStatus(int right, int cy, const ViewState &v) {
    int x = right;
    char buf[8];
    if (v.battery >= 0) {
        x = iconBattery(x, cy, v.battery) - 8;
        snprintf(buf, sizeof(buf), "%d%%", v.battery);
        drawTextRight(x, cy + 8, &FreeSans12pt7b, buf, INK);
        x -= textWidth(&FreeSans12pt7b, buf) + 6;
    }
    if (v.usb) {
        iconBolt(x - 6, cy - 1);
        x -= 22;
    }
    if (v.sdCard) x = iconSdCard(x, cy - 1) - 8;
    return x;
}

static void drawDateTime(const ViewState &v) {
    if (v.clockMinute < 0) return;
    char date[16], hhmm[8];
    time_t now = (time_t)v.clockMinute * 60;
    formatLocal(hhmm, sizeof(hhmm), "%H:%M", now);
    formatLocal(date, sizeof(date), "%e %b", now);
    drawTextRight(SW - MARGIN, 84, &FreeSansBold12pt7b, hhmm, INK);
    drawTextRight(SW - MARGIN - textWidth(&FreeSansBold12pt7b, hhmm) - 10, 84, &FreeSans12pt7b, date, INK);
}

// "zvukopis", status, calendar and settings buttons; moment count and clock below
static void drawMainHeader(const ViewState &v) {
    drawText(MARGIN, 50, &FreeSansBold18pt7b, "zvukopis", INK);
    int settingsX = SW - MARGIN - ICON_BOX, calendarX = settingsX - ICON_BOX - 8;
    drawHeaderButton(settingsX, 38, v.selected == ROW_SETTINGS, iconGear);
    drawHeaderButton(calendarX, 38, v.selected == ROW_CALENDAR, iconCalendar);
    drawStatus(calendarX - 14, 38, v);

    // "2 moments  •  3:04 left"
    char buf[32];
    if (v.momentCount == 1) snprintf(buf, sizeof(buf), "1 moment");
    else snprintf(buf, sizeof(buf), "%d moments", v.momentCount);
    drawText(MARGIN, 84, &FreeSans12pt7b, buf, INK);
    int x = MARGIN + textWidth(&FreeSans12pt7b, buf) + 14;
    canvas->fillCircle(x, 78, 2, INK);
    if (v.secondsLeft < 1) {
        snprintf(buf, sizeof(buf), v.sdCard ? "card full" : "memory full");
    } else {
        char left[16];
        formatTimeLeft(left, sizeof(left), v.secondsLeft);
        snprintf(buf, sizeof(buf), "%s left", left);
    }
    drawText(x + 14, 84, &FreeSans12pt7b, buf, INK);
    drawDateTime(v);
    canvas->fillRect(MARGIN, HEADER_LINE_Y, SW - 2 * MARGIN, 2, INK);
}

// Back button and title; a subtitle and the clock below
static void drawSubHeader(const ViewState &v, const char *title, const char *subtitle, bool canGoBack = true) {
    int x = MARGIN;
    if (canGoBack) {
        bool isSelected = v.selected == ROW_BACK;
        const int w = 92, y = 18;
        uint16_t fg = isSelected ? PAPER : INK;
        if (isSelected) canvas->fillRoundRect(x, y, w, ICON_BOX, 9, INK);
        else canvas->drawRoundRect(x, y, w, ICON_BOX, 9, INK);
        canvas->fillTriangle(x + 12, y + 20, x + 22, y + 11, x + 22, y + 29, fg);
        drawText(x + 30, y + 28, &FreeSans12pt7b, "Back", fg);
        x += w + 14;
    }
    drawText(x, 50, &FreeSansBold18pt7b, title, INK);
    drawStatus(SW - MARGIN, 38, v);
    if (subtitle) drawText(MARGIN, 84, &FreeSans12pt7b, subtitle, INK);
    drawDateTime(v);
    canvas->fillRect(MARGIN, HEADER_LINE_Y, SW - 2 * MARGIN, 2, INK);
}

static void drawNewRecordingRow(int y, uint16_t fg, bool isSelected, const ViewState &v) {
    int cy = y + ROW_H / 2;
    iconRecord(MARGIN + 14, cy, fg);
    drawText(MARGIN + 44, cy + 8, &FreeSansBold12pt7b, "New recording", fg);
    if (isSelected) {
        const char *hint = v.secondsLeft < 1 ? (v.sdCard ? "card full" : "memory full") : "click to record";
        drawTextRight(SW - MARGIN, cy + 8, &FreeSans12pt7b, hint, fg);
    }
}

// Two lines: name, date, tag and duration, then a waveform. While playing,
// the part not yet played is drawn hollow.
static void drawMomentRow(int index, int y, uint16_t fg, const ViewState &v, bool showDate) {
    Moment moment;
    if (!momentGet(index, moment)) return;
    bool playing = v.audio == AUDIO_PLAYING && v.playingIndex == index;

    int lineY = y + 40;
    if (playing) iconStop(MARGIN + 14, lineY - 8, fg);
    else iconPlay(MARGIN + 14, lineY - 8, fg);

    char buf[32];
    snprintf(buf, sizeof(buf), "Moment %d", moment.number);
    drawText(MARGIN + 44, lineY, &FreeSansBold12pt7b, buf, fg);
    if (moment.recordedAt && !playing) { // while playing, the speed takes its place
        int x = MARGIN + 44 + textWidth(&FreeSansBold12pt7b, buf) + 12;
        formatLocal(buf, sizeof(buf), showDate ? "%e %b %H:%M" : "%H:%M:%S", moment.recordedAt);
        drawText(x, lineY, &FreeSans12pt7b, buf, fg);
    }

    char total[12];
    formatTime(total, sizeof(total), momentSeconds(moment));
    if (playing) {
        char elapsed[12];
        formatTime(elapsed, sizeof(elapsed), v.playSeconds);
        if (v.speedTenths != 10) {
            snprintf(buf, sizeof(buf), "%d.%dx  %s", v.speedTenths / 10, v.speedTenths % 10, elapsed);
        } else {
            snprintf(buf, sizeof(buf), "%s / %s", elapsed, total);
        }
    } else {
        snprintf(buf, sizeof(buf), "%s", total);
    }
    drawTextRight(SW - MARGIN, lineY, &FreeSans12pt7b, buf, fg);
    if (moment.tag != TAG_NONE) {
        iconTag(moment.tag, SW - MARGIN - textWidth(&FreeSans12pt7b, buf) - 20, lineY - 8, 10, fg);
    }

    int waveY = y + 68;
    int progressBin = WAVEFORM_BINS;
    if (playing) progressBin = (int)((float)v.playSeconds * SAMPLE_RATE / moment.length * WAVEFORM_BINS);
    for (int b = 0; b < WAVEFORM_BINS; b++) {
        int x = WAVE_X + b * WAVE_PITCH;
        int h = 1 + moment.waveform[b] * WAVE_HALF_H / 255;
        if (b < progressBin) canvas->fillRect(x, waveY - h, 3, 2 * h + 1, fg);
        else canvas->drawRect(x, waveY - h, 3, 2 * h + 1, fg);
    }
}

// Scrolling rows. Row r is New recording (main list, r == 0) or a moment.
static void drawRows(const ViewState &v, int rows, bool withNewRecording, const int *dayMoments) {
    static int scrollTop = 0;
    int sel = max(0, v.selected);
    if (sel < scrollTop) scrollTop = sel;
    if (sel >= scrollTop + VISIBLE_ROWS) scrollTop = sel - VISIBLE_ROWS + 1;
    scrollTop = constrain(scrollTop, 0, max(0, rows - VISIBLE_ROWS));

    for (int i = 0; i < VISIBLE_ROWS && scrollTop + i < rows; i++) {
        int row = scrollTop + i;
        int y = LIST_TOP + i * ROW_H;
        bool isSelected = row == v.selected;
        uint16_t fg = isSelected ? PAPER : INK;
        if (isSelected) canvas->fillRoundRect(MARGIN - 12, y + 4, SW - 2 * MARGIN + 24, ROW_H - 8, 12, INK);

        if (withNewRecording && row == 0) drawNewRecordingRow(y, fg, isSelected, v);
        else if (withNewRecording) drawMomentRow(row - 1, y, fg, v, true);
        else drawMomentRow(dayMoments[row], y, fg, v, false);

        bool nextSelected = row + 1 == v.selected;
        if (!isSelected && !nextSelected && i < VISIBLE_ROWS - 1 && row + 1 < rows) {
            dottedLine(MARGIN, SW - MARGIN, y + ROW_H);
        }
    }

    // Scroll indicators above and below the list
    int cx = SW / 2;
    if (scrollTop > 0) canvas->fillTriangle(cx - 8, LIST_TOP - 3, cx + 8, LIST_TOP - 3, cx, LIST_TOP - 11, INK);
    if (scrollTop + VISIBLE_ROWS < rows) {
        int bottom = LIST_TOP + VISIBLE_ROWS * ROW_H;
        canvas->fillTriangle(cx - 8, bottom + 3, cx + 8, bottom + 3, cx, bottom + 11, INK);
    }
}

static void drawList(const ViewState &v) {
    drawMainHeader(v);
    drawRows(v, 1 + v.momentCount, true, nullptr);
    if (v.momentCount == 0) {
        int cx = SW / 2, y = LIST_TOP + ROW_H + 110;
        drawTextCentered(cx, y, &FreeSansBold12pt7b, "No moments yet", INK);
        drawTextCentered(cx, y + 38, &FreeSans12pt7b, "Click New recording to capture", INK);
        drawTextCentered(cx, y + 66, &FreeSans12pt7b, "the sound of this moment", INK);
    }
}

static void drawDay(const ViewState &v) {
    int n = momentsOnDay(v.calendarDay, displayDayMoments, MAX_MOMENTS);
    tm day = {};
    day.tm_year = v.calendarDay / 10000 - 1900;
    day.tm_mon = v.calendarDay / 100 % 100 - 1;
    day.tm_mday = v.calendarDay % 100;
    day.tm_hour = 12;
    mktime(&day); // fills in the weekday
    char title[24], subtitle[48];
    strftime(title, sizeof(title), "%a %e %b", &day);
    int seconds = 0;
    Moment moment;
    for (int i = 0; i < n; i++) {
        if (momentGet(displayDayMoments[i], moment)) seconds += momentSeconds(moment);
    }
    char total[12];
    formatTime(total, sizeof(total), seconds);
    snprintf(subtitle, sizeof(subtitle), "%d moment%s  -  %s", n, n == 1 ? "" : "s", total);
    drawSubHeader(v, title, subtitle);
    drawRows(v, n, false, displayDayMoments);
}

// Month grid around the chosen day; days with moments are bold with a bar
// under the number, the chosen one inverted
static void drawCalendar(const ViewState &v) {
    const DayIndex &index = daysWithMoments(displayDays);
    char subtitle[48];
    int total = 0;
    for (int d = 0; d < index.count; d++) total += index.days[d].count;
    snprintf(subtitle, sizeof(subtitle), "%d moment%s on %d day%s", total, total == 1 ? "" : "s",
             index.count, index.count == 1 ? "" : "s");
    drawSubHeader(v, "Calendar", subtitle);

    int cx = SW / 2;
    if (index.count == 0) {
        drawTextCentered(cx, 300, &FreeSansBold12pt7b, "No dated moments yet", INK);
        drawTextCentered(cx, 338, &FreeSans12pt7b, "Moments get a date once", INK);
        drawTextCentered(cx, 366, &FreeSans12pt7b, "the clock is set", INK);
        return;
    }
    int key = v.calendarDay;
    int year = key / 10000, month = key / 100 % 100;
    tm first = {};
    first.tm_year = year - 1900;
    first.tm_mon = month - 1;
    first.tm_mday = 1;
    first.tm_hour = 12;
    mktime(&first);
    char buf[48];
    strftime(buf, sizeof(buf), "%B %Y", &first);
    drawTextCentered(cx, 150, &FreeSansBold18pt7b, buf, INK);
    // Other months with moments, either side
    if (index.days[0].key / 100 < key / 100) canvas->fillTriangle(MARGIN, 140, MARGIN + 12, 130, MARGIN + 12, 150, INK);
    if (index.days[index.count - 1].key / 100 > key / 100) {
        canvas->fillTriangle(SW - MARGIN, 140, SW - MARGIN - 12, 130, SW - MARGIN - 12, 150, INK);
    }

    const int cellW = (SW - 2 * MARGIN) / 7, cellH = 62, gridTop = 196;
    const char *weekdays[7] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
    for (int c = 0; c < 7; c++) {
        drawTextCentered(MARGIN + c * cellW + cellW / 2, 184, &FreeSans9pt7b, weekdays[c], INK);
    }
    int offset = (first.tm_wday + 6) % 7; // Monday first
    int daysInMonth = 31;
    while (daysInMonth > 28) {
        tm probe = first;
        probe.tm_mday = daysInMonth;
        mktime(&probe);
        if (probe.tm_mon == month - 1) break;
        daysInMonth--;
    }
    int today = v.clockMinute >= 0 ? dayKey((time_t)v.clockMinute * 60) : 0;
    for (int d = 1; d <= daysInMonth; d++) {
        int cell = offset + d - 1;
        int x = MARGIN + cell % 7 * cellW, y = gridTop + cell / 7 * cellH;
        int dayCx = x + cellW / 2;
        int thisKey = key / 100 * 100 + d;
        int found = findDay(index, thisKey);
        bool chosen = thisKey == key && v.selected != ROW_BACK;
        uint16_t fg = INK;
        if (chosen) {
            canvas->fillRoundRect(x + 3, y + 2, cellW - 6, cellH - 4, 10, INK);
            fg = PAPER;
        } else if (thisKey == today) {
            canvas->drawRoundRect(x + 3, y + 2, cellW - 6, cellH - 4, 10, INK);
        }
        snprintf(buf, sizeof(buf), "%d", d);
        drawTextCentered(dayCx, y + 32, found >= 0 ? &FreeSansBold12pt7b : &FreeSans12pt7b, buf, fg);
        if (found >= 0) {
            // A dot per moment, up to four
            int dots = min(4, index.days[found].count);
            for (int i = 0; i < dots; i++) canvas->fillCircle(dayCx - (dots - 1) * 5 + i * 10, y + 46, 3, fg);
        }
    }

    // The chosen day
    int found = findDay(index, key);
    if (found < 0) return;
    const DayInfo &day = index.days[found];
    tm chosenDay = first;
    chosenDay.tm_mday = key % 100;
    mktime(&chosenDay);
    strftime(buf, sizeof(buf), "%A %e %B", &chosenDay);
    int y = gridTop + 6 * cellH + 34;
    drawText(MARGIN, y, &FreeSansBold12pt7b, buf, INK);
    char total2[12];
    formatTime(total2, sizeof(total2), day.seconds);
    snprintf(buf, sizeof(buf), "%d moment%s, %s", day.count, day.count == 1 ? "" : "s", total2);
    drawText(MARGIN, y + 30, &FreeSans12pt7b, buf, INK);
    int x = SW - MARGIN - 12;
    for (int t = TAG_COUNT - 1; t > TAG_NONE; t--) {
        if (day.tags & (1 << t)) {
            iconTag(t, x, y + 12, 11, INK);
            x -= 30;
        }
    }
}

static void settingsRowText(SettingsRow row, const ViewState &v, const char *&title, char *detail, size_t len,
                            bool &enabled) {
    enabled = true;
    switch (row) {
    case SET_SYNC:
        title = "Sync clock";
        switch (v.clockSync) {
        case CLOCK_SYNCING: snprintf(detail, len, "Syncing..."); break;
        case CLOCK_SYNC_DONE: snprintf(detail, len, "Clock synced"); break;
        case CLOCK_SYNC_NO_WIFI: snprintf(detail, len, "Wi-Fi network not found"); break;
        case CLOCK_SYNC_FAILED: snprintf(detail, len, "No answer from the time server"); break;
        default:
            if (clockSyncAvailable()) snprintf(detail, len, "Time from the internet, over Wi-Fi");
            else snprintf(detail, len, "Wi-Fi isn't set up (wifi_config.h)");
            enabled = clockSyncAvailable();
            break;
        }
        break;
    case SET_GAIN:
        title = "Mic gain";
        if (v.gainSetting == GAIN_AUTO) snprintf(detail, len, "Auto, now %d dB", v.gainNow);
        else snprintf(detail, len, "%d dB", v.gainSetting);
        break;
    case SET_TRANSFER:
        title = "File transfer";
        snprintf(detail, len, "Download moments in a browser");
        break;
    case SET_CARD_READER:
        title = "USB card reader";
        enabled = v.sdCard && v.usb;
        snprintf(detail, len, !v.sdCard ? "No SD card" : !v.usb ? "Connect USB to a computer first"
                                                                 : "Open the SD card on the computer");
        break;
    case SET_FORMAT:
        title = "Format SD card";
        enabled = v.sdCard && v.formatStatus != 2;
        if (v.formatStatus == 2) snprintf(detail, len, "Formatting...");
        else if (v.formatStatus == 1) snprintf(detail, len, "Formatted");
        else if (v.formatStatus == -1) snprintf(detail, len, "Formatting failed");
        else if (!v.sdCard) snprintf(detail, len, "No SD card");
        else snprintf(detail, len, "Erases everything on the card");
        break;
    default:
        title = "";
        detail[0] = 0;
        break;
    }
}

static void drawSettings(const ViewState &v) {
    drawSubHeader(v, "Settings", nullptr);
    const int rowH = 80;
    for (int r = 0; r < SET_ROWS; r++) {
        int y = LIST_TOP + r * rowH;
        bool isSelected = r == v.selected;
        uint16_t fg = isSelected ? PAPER : INK;
        if (isSelected) canvas->fillRoundRect(MARGIN - 12, y + 4, SW - 2 * MARGIN + 24, rowH - 8, 12, INK);
        else if (r > 0 && r - 1 != v.selected) dottedLine(MARGIN, SW - MARGIN, y);
        const char *title;
        char detail[48];
        bool enabled;
        settingsRowText((SettingsRow)r, v, title, detail, sizeof(detail), enabled);
        drawText(MARGIN, y + 36, enabled ? &FreeSansBold12pt7b : &FreeSans12pt7b, title, fg);
        drawText(MARGIN, y + 63, &FreeSans9pt7b, detail, fg);
    }

    // Card and memory
    int y = LIST_TOP + SET_ROWS * rowH + 44;
    char buf[64], total[16], free[16];
    if (v.sdCard) {
        formatBytes(total, sizeof(total), storageTotalBytes());
        formatBytes(free, sizeof(free), storageFreeBytes());
        iconSdCard(MARGIN + 16, y - 6);
        snprintf(buf, sizeof(buf), "%s card, %s free", total, free);
        drawText(MARGIN + 30, y, &FreeSans12pt7b, buf, INK);
    } else {
        drawText(MARGIN, y, &FreeSans12pt7b, "No SD card: moments are kept", INK);
        drawText(MARGIN, y + 28, &FreeSans12pt7b, "in memory and lost on restart", INK);
    }
}

static void drawTransfer(const ViewState &v) {
    drawSubHeader(v, "File transfer", "Wi-Fi is on", false);
    TransferInfo info = transferInfo();
    int cx = SW / 2, y = 250;
    iconWifi(cx, y);
    y += 70;
    if (info.state == TRANSFER_CONNECTING) {
        drawTextCentered(cx, y, &FreeSansBold18pt7b, "Connecting...", INK);
        return;
    }
    if (info.state != TRANSFER_READY) return;
    char buf[64];
    if (info.hotspot) {
        drawTextCentered(cx, y, &FreeSans12pt7b, "Join the Wi-Fi network", INK);
        drawTextCentered(cx, y + 42, &FreeSansBold18pt7b, info.network, INK);
        snprintf(buf, sizeof(buf), "password  %s", info.password);
        drawTextCentered(cx, y + 76, &FreeSans12pt7b, buf, INK);
        y += 130;
    } else {
        snprintf(buf, sizeof(buf), "On the network %s,", info.network);
        drawTextCentered(cx, y, &FreeSans12pt7b, buf, INK);
        y += 42;
    }
    drawTextCentered(cx, y, &FreeSans12pt7b, "open in a browser", INK);
    snprintf(buf, sizeof(buf), "http://%s", info.address);
    drawTextCentered(cx, y + 44, &FreeSansBold18pt7b, buf, INK);
    if (!info.hotspot) drawTextCentered(cx, y + 78, &FreeSans12pt7b, "or http://zvukopis.local", INK);
    y += 140;
    if (v.transferRequests > 0) {
        snprintf(buf, sizeof(buf), "%lu request%s served", v.transferRequests, v.transferRequests == 1 ? "" : "s");
        drawTextCentered(cx, y, &FreeSans12pt7b, buf, INK);
    }
    drawTextCentered(cx, y + 34, &FreeSans9pt7b, "Stops by itself after 10 minutes without use", INK);
}

static void drawCardReader(const ViewState &v) {
    drawSubHeader(v, "USB card reader", "The SD card is open on the computer", false);
    int cx = SW / 2;
    iconSdCard(cx + 24, 260, 3);
    if (v.cardEjected) {
        drawTextCentered(cx, 380, &FreeSansBold18pt7b, "Ejected", INK);
        drawTextCentered(cx, 418, &FreeSans12pt7b, "Safe to leave", INK);
    } else {
        drawTextCentered(cx, 380, &FreeSansBold18pt7b, "Connected", INK);
        drawTextCentered(cx, 418, &FreeSans12pt7b, "Eject the drive on the computer", INK);
        drawTextCentered(cx, 446, &FreeSans12pt7b, "before leaving", INK);
    }
    drawTextCentered(cx, 520, &FreeSans9pt7b, "Leaving restarts zvukopis, which reads", INK);
    drawTextCentered(cx, 542, &FreeSans9pt7b, "the card's moments again", INK);
}

// Drawn once when recording starts and left alone until it stops: the panel's
// drive voltages couple into the microphone, so a refresh adds about 0.5 s of
// static to the audio (measured: ~2.7x the noise floor, whatever the mic gain).
static void drawRecordingView(const ViewState &v) {
    drawMainHeader(v);
    int cx = SW / 2;
    bool saving = v.audio == AUDIO_SAVING;
    if (saving) canvas->drawCircle(cx, 270, 26, INK);
    else canvas->fillCircle(cx, 270, 26, INK);
    drawTextCentered(cx, 368, &FreeSansBold24pt7b, saving ? "Saving" : "Recording", INK);

    char limit[12], started[12], buf[48];
    formatTime(limit, sizeof(limit), max(1, (int)audioRecordingLimitSeconds()));
    if (v.clockMinute >= 0) {
        formatLocal(started, sizeof(started), "%H:%M", (time_t)v.clockMinute * 60);
        snprintf(buf, sizeof(buf), "started %s  -  up to %s", started, limit);
    } else {
        snprintf(buf, sizeof(buf), "up to %s", limit);
    }
    drawTextCentered(cx, 412, &FreeSans12pt7b, buf, INK);
    if (!saving) {
        drawTextCentered(cx, 500, &FreeSans9pt7b, "The screen stays still while recording,", INK);
        drawTextCentered(cx, 522, &FreeSans9pt7b, "so it doesn't add noise to the audio.", INK);
    }
}

// Shades everything below the header so a dialog stands out; returns nothing
static void shadeBehindDialog() {
    for (int y = LIST_TOP; y < FOOTER_LINE_Y - 4; y += 2) {
        for (int x = (y / 2 % 2) * 2; x < SW; x += 4) canvas->drawPixel(x, y, INK);
    }
}

static void dialogBox(int y, int h) {
    const int w = SW - 2 * MARGIN - 16, x = (SW - w) / 2;
    canvas->fillRoundRect(x - 6, y - 6, w + 12, h + 12, 20, PAPER);
    canvas->fillRoundRect(x, y, w, h, 16, INK);
    canvas->fillRoundRect(x + 3, y + 3, w - 6, h - 6, 13, PAPER);
}

// Cancel / <action>, chosen with up/down
static void drawConfirm(const ViewState &v, void (*icon)(), const char *title, const char *detail, const char *action) {
    shadeBehindDialog();
    const int w = SW - 2 * MARGIN - 16, h = 250;
    const int x = (SW - w) / 2, y = 268, cx = SW / 2;
    dialogBox(y, h);
    icon();
    drawTextCentered(cx, y + 134, &FreeSansBold18pt7b, title, INK);
    drawTextCentered(cx, y + 166, &FreeSans12pt7b, detail, INK);

    const char *labels[2] = {"Cancel", action};
    const int gap = 20, bw = (w - 3 * gap) / 2, bh = 48, by = y + h - bh - 18;
    for (int i = 0; i < 2; i++) {
        int bx = x + gap + i * (bw + gap);
        bool chosen = i == v.overlayChoice;
        if (chosen) {
            canvas->fillRoundRect(bx, by, bw, bh, 10, INK);
        } else {
            canvas->drawRoundRect(bx, by, bw, bh, 10, INK);
            canvas->drawRoundRect(bx + 1, by + 1, bw - 2, bh - 2, 9, INK);
        }
        drawTextCentered(bx + bw / 2, by + 32, &FreeSansBold12pt7b, labels[i], chosen ? PAPER : INK);
    }
}

static void drawDeleteDialog(const ViewState &v) {
    int index = momentIndexByNumber(v.menuMoment);
    Moment moment;
    if (!momentGet(index, moment)) return;
    char title[32], detail[48], duration[12];
    snprintf(title, sizeof(title), "Delete Moment %d?", moment.number);
    formatTime(duration, sizeof(duration), momentSeconds(moment));
    snprintf(detail, sizeof(detail), "%s  -  can't be undone", duration);
    drawConfirm(v, [] { iconTrash(SW / 2, 268 + 58); }, title, detail, "Delete");
}

static void drawFormatDialog(const ViewState &v) {
    char detail[48];
    int onCard = 0;
    Moment moment;
    for (int i = 0; momentGet(i, moment); i++) {
        if (!moment.samples) onCard++;
    }
    snprintf(detail, sizeof(detail), "Erases %d moment%s and all else", onCard, onCard == 1 ? "" : "s");
    drawConfirm(v, [] { iconSdCard(SW / 2 + 24, 268 + 62, 3); }, "Format SD card?", detail, "Format");
}

// Auto, or a fixed gain
static void drawGainPicker(const ViewState &v) {
    shadeBehindDialog();
    const int itemH = 40, y = 126, h = 70 + GAIN_CHOICE_COUNT * itemH + 12;
    const int w = SW - 2 * MARGIN - 16, x = (SW - w) / 2;
    dialogBox(y, h);
    drawText(x + 24, y + 44, &FreeSansBold18pt7b, "Mic gain", INK);
    for (int i = 0; i < GAIN_CHOICE_COUNT; i++) {
        int iy = y + 62 + i * itemH;
        bool chosen = i == v.overlayChoice;
        uint16_t fg = chosen ? PAPER : INK;
        if (chosen) canvas->fillRoundRect(x + 12, iy, w - 24, itemH - 4, 10, INK);
        char label[48];
        if (GAIN_CHOICES[i] == GAIN_AUTO) snprintf(label, sizeof(label), "Auto  (adjusts while recording)");
        else snprintf(label, sizeof(label), "%d dB%s", GAIN_CHOICES[i], GAIN_CHOICES[i] == 54 ? "  (quiet places)" : GAIN_CHOICES[i] == 0 ? "  (very loud)" : "");
        drawText(x + 56, iy + 26, &FreeSans12pt7b, label, fg);
        if (GAIN_CHOICES[i] == v.gainSetting) canvas->fillCircle(x + 34, iy + 18, 5, fg);
    }
}

// Tag the moment, or delete it
static void drawMenu(const ViewState &v) {
    int index = momentIndexByNumber(v.menuMoment);
    Moment moment;
    if (!momentGet(index, moment)) return;
    shadeBehindDialog();
    const int itemH = 50, y = 160, h = 76 + MENU_ITEMS * itemH + 14;
    const int w = SW - 2 * MARGIN - 16, x = (SW - w) / 2;
    dialogBox(y, h);
    char title[32];
    snprintf(title, sizeof(title), "Moment %d", moment.number);
    drawText(x + 24, y + 46, &FreeSansBold18pt7b, title, INK);
    drawTextRight(x + w - 24, y + 46, &FreeSans12pt7b, "is about", INK);

    for (int i = 0; i < MENU_ITEMS; i++) {
        int iy = y + 70 + i * itemH;
        bool chosen = i == v.overlayChoice;
        uint16_t fg = chosen ? PAPER : INK;
        if (chosen) canvas->fillRoundRect(x + 12, iy, w - 24, itemH - 4, 10, INK);
        int cy = iy + (itemH - 4) / 2;
        const char *label;
        if (i < MENU_DELETE) {
            int tag = TAG_HUMAN + i;
            iconTag(tag, x + 44, cy, 13, fg);
            label = TAG_LABELS[tag];
            if (moment.tag == tag) {
                // Check mark: choosing it again removes the tag
                canvas->drawLine(x + w - 56, cy, x + w - 48, cy + 8, fg);
                canvas->drawLine(x + w - 56, cy + 1, x + w - 48, cy + 9, fg);
                canvas->drawLine(x + w - 48, cy + 8, x + w - 34, cy - 8, fg);
                canvas->drawLine(x + w - 48, cy + 9, x + w - 34, cy - 7, fg);
            }
        } else if (i == MENU_DELETE) {
            label = "Delete...";
        } else {
            label = "Cancel";
        }
        drawText(x + 76, cy + 8, i < MENU_DELETE ? &FreeSansBold12pt7b : &FreeSans12pt7b, label, fg);
    }
}

// A quote about listening fills the screen, as large as it fits; below it,
// that zvukopis is asleep, since when, and what it holds
static void drawSleepScreen(const ViewState &v) {
    const int cx = SW / 2, top = 50, bottom = 600, width = SW - 2 * MARGIN - 8;
    const Quote &quote = QUOTES[v.quote % QUOTE_COUNT];
    struct Size {
        const GFXfont *font;
        int lineHeight;
        const GFXfont *sourceFont;
        int sourceLineHeight;
    };
    static const Size sizes[] = {
        {&FreeSerifItalic18pt7b, 44, &FreeSans12pt7b, 30},
        {&FreeSerifItalic12pt7b, 31, &FreeSans9pt7b, 24},
        {&FreeSerifItalic9pt7b, 24, &FreeSans9pt7b, 22},
    };
    const Size *size = &sizes[0];
    int height = 0;
    for (const Size &s : sizes) {
        size = &s;
        height = drawWrapped(cx, 0, width, s.lineHeight, s.font, quote.text, false) + 16 +
                 drawWrapped(cx, 0, width, s.sourceLineHeight, s.sourceFont, quote.source, false);
        if (height <= bottom - top) break;
    }
    int y = top + max(0, (bottom - top - height) / 2) + size->lineHeight - 10;
    y = drawWrapped(cx, y, width, size->lineHeight, size->font, quote.text);
    drawWrapped(cx, y + 16 - size->lineHeight + size->sourceLineHeight, width, size->sourceLineHeight,
                size->sourceFont, quote.source);

    canvas->fillRect(cx - 30, 622, 60, 2, INK);
    char buf[64];
    if (v.clockMinute >= 0) {
        char since[12];
        formatLocal(since, sizeof(since), "%H:%M", (time_t)v.clockMinute * 60);
        snprintf(buf, sizeof(buf), "Asleep since %s", since);
    } else {
        snprintf(buf, sizeof(buf), "Asleep");
    }
    drawTextCentered(cx, 656, &FreeSansBold12pt7b, buf, INK);
    const char *where = v.sdCard ? "on SD card" : "in memory";
    int n = snprintf(buf, sizeof(buf), "%d moment%s %s", v.momentCount, v.momentCount == 1 ? "" : "s", where);
    if (v.battery >= 0) snprintf(buf + n, sizeof(buf) - n, "  -  battery %d%%", v.battery);
    drawTextCentered(cx, 686, &FreeSans12pt7b, buf, INK);
}

// Context hints in a 2x2 grid
static void drawFooter(const ViewState &v) {
    struct Hint {
        HintIcon icon;
        const char *label;
    } hints[4];
    int n = 0;
    bool playing = v.audio == AUDIO_PLAYING;
    if (v.sleeping) {
        hints[n++] = {HINT_PWR, "Hold 2 s to wake"};
    } else if (v.audio == AUDIO_SAVING || v.audio == AUDIO_FORMATTING) {
        // nothing to press meanwhile
    } else if (v.audio == AUDIO_RECORDING) {
        hints[n++] = {HINT_CLICK, "Stop recording"};
    } else if (v.overlay != OVERLAY_NONE) {
        hints[n++] = {HINT_UPDOWN, "Choose"};
        hints[n++] = {HINT_CLICK, "Confirm"};
    } else if (v.screen == SCREEN_TRANSFER) {
        hints[n++] = {HINT_CLICK, "Stop file transfer"};
    } else if (v.screen == SCREEN_CARD_READER) {
        hints[n++] = {HINT_CLICK, "Leave (restarts)"};
    } else if (v.screen == SCREEN_SETTINGS) {
        hints[n++] = {HINT_UPDOWN, "Select"};
        hints[n++] = {HINT_CLICK, v.selected == ROW_BACK ? "Back" : "Choose"};
        hints[n++] = {HINT_HOLD_CLICK, "Back"};
        hints[n++] = {HINT_PWR, "Hold: sleep"};
    } else if (v.screen == SCREEN_CALENDAR) {
        hints[n++] = {HINT_UPDOWN, "Day"};
        hints[n++] = {HINT_CLICK, v.selected == ROW_BACK ? "Back" : "Open day"};
        hints[n++] = {HINT_HOLD_CLICK, "Back"};
        hints[n++] = {HINT_PWR, "Hold: sleep"};
    } else if (playing && (v.screen == SCREEN_LIST || v.screen == SCREEN_DAY)) {
        hints[n++] = {HINT_UPDOWN, "Hold: speed"};
        hints[n++] = {HINT_CLICK, "Stop"};
        hints[n++] = {HINT_HOLD_CLICK, "Tag, delete"};
        hints[n++] = {HINT_PWR, "Hold: sleep"};
    } else if (v.selected < 0) {
        const char *open = v.screen == SCREEN_DAY ? "Back"
                           : v.selected == ROW_SETTINGS ? "Settings"
                                                        : "Calendar";
        hints[n++] = {HINT_UPDOWN, "Select"};
        hints[n++] = {HINT_CLICK, open};
        hints[n++] = {HINT_PWR, "Hold: sleep"};
    } else if (v.screen == SCREEN_LIST && v.selected == 0) {
        hints[n++] = {HINT_UPDOWN, "Select"};
        hints[n++] = {HINT_CLICK, "Record"};
        hints[n++] = {HINT_PWR, "Hold: sleep"};
    } else {
        hints[n++] = {HINT_UPDOWN, "Select"};
        hints[n++] = {HINT_CLICK, "Play"};
        hints[n++] = {HINT_HOLD_CLICK, "Tag, delete"};
        hints[n++] = {HINT_PWR, "Hold: sleep"};
    }

    canvas->drawFastHLine(MARGIN, FOOTER_LINE_Y, SW - 2 * MARGIN, INK);
    for (int i = 0; i < n; i++) {
        int x = MARGIN + (i % 2) * (SW / 2 - MARGIN + 8);
        int cy = FOOTER_LINE_Y + 34 + (i / 2) * 40;
        drawHint(x, cy, hints[i].icon, hints[i].label);
    }
}

static void render(const ViewState &v) {
    canvas->fillScreen(PAPER);
    if (v.sleeping) {
        drawSleepScreen(v);
        drawFooter(v);
        return;
    }
    if (v.audio == AUDIO_RECORDING || v.audio == AUDIO_SAVING) {
        drawRecordingView(v);
    } else {
        switch (v.screen) {
        case SCREEN_LIST: drawList(v); break;
        case SCREEN_CALENDAR: drawCalendar(v); break;
        case SCREEN_DAY: drawDay(v); break;
        case SCREEN_SETTINGS: drawSettings(v); break;
        case SCREEN_TRANSFER: drawTransfer(v); break;
        case SCREEN_CARD_READER: drawCardReader(v); break;
        }
        switch (v.overlay) {
        case OVERLAY_MENU: drawMenu(v); break;
        case OVERLAY_CONFIRM_DELETE: drawDeleteDialog(v); break;
        case OVERLAY_CONFIRM_FORMAT: drawFormatDialog(v); break;
        case OVERLAY_GAIN: drawGainPicker(v); break;
        }
    }
    drawFooter(v);
}

// --- Panel output ---

static void pushPartial(const ViewState &v) {
    uint32_t start = millis();
    Serial.printf("[display] %lu ms: refresh (screen %d, selected %d, moments %d, audio %d, overlay %d)\n",
                  start, v.screen, v.selected, v.momentCount, v.audio, v.overlay);
    EPD_3IN97_Display_Partial(canvas->getBuffer(), 0, 0, EPD_3IN97_WIDTH, EPD_3IN97_HEIGHT);
    Serial.printf("[display] partial refresh took %lu ms\n", millis() - start);
}

static void pushFull() {
    uint32_t start = millis();
    EPD_3IN97_Init();
    EPD_3IN97_Display_Base(canvas->getBuffer()); // writes both panel buffers, which partial refreshes build on
    Serial.printf("[display] %lu ms: full refresh took %lu ms\n", start, millis() - start);
}

// Raw panel buffer (landscape, 1 bit per pixel) as base64, with the rotation so a viewer can turn it upright
static void dumpScreenshot() {
    const size_t inLen = EPD_3IN97_WIDTH / 8 * EPD_3IN97_HEIGHT;
    size_t capacity = 4 * ((inLen + 2) / 3) + 1, outLen = 0;
    unsigned char *out = (unsigned char *)ps_malloc(capacity);
    if (!out) return;
    mbedtls_base64_encode(out, capacity, &outLen, canvas->getBuffer(), inLen);
    Serial.printf("SCREENSHOT-BEGIN rotation=%d\n", SCREEN_ROTATION);
    for (size_t i = 0; i < outLen; i += 76) {
        Serial.write(out + i, min((size_t)76, outLen - i));
        Serial.println();
    }
    Serial.println("SCREENSHOT-END");
    free(out);
}

static void displayTask(void *) {
    ViewState shown = currentView();
    int partials = 0;
    for (;;) {
        uint32_t iterationStart = millis();
        ViewState v = currentView();
        bool pushed = true;
        if (v.sleeping != shown.sleeping) {
            // Going to sleep: full refresh for a clean sleep screen, then the panel's
            // deep sleep. Waking: the full refresh's init also wakes the panel.
            Serial.printf("[display] %s\n", v.sleeping ? "sleep screen" : "waking");
            render(v);
            pushFull();
            if (v.sleeping) EPD_3IN97_Sleep();
            panelAsleep = v.sleeping;
            shown = v;
            partials = 0;
        } else if (v.sleeping) {
            pushed = false; // leave the sleeping panel alone
        } else if (v.audio == AUDIO_RECORDING && shown.audio == AUDIO_RECORDING) {
            pushed = false; // keep the panel still while recording; see drawRecordingView
        } else if (memcmp(&v, &shown, sizeof(v)) != 0) {
            uint32_t start = millis();
            render(v);
            Serial.printf("[display] render took %lu ms\n", millis() - start);
            pushPartial(v);
            shown = v;
            partials++;
        } else if (partials >= FULL_REFRESH_EVERY && v.audio == AUDIO_IDLE && v.overlay == OVERLAY_NONE &&
                   millis() - lastActivity > FULL_REFRESH_IDLE_MS) {
            pushFull();
            partials = 0;
        } else {
            pushed = false;
        }
        if (pushed) quietSince = 0;
        else if (quietSince == 0) quietSince = max(iterationStart, (uint32_t)1);
        if (screenshotRequested) {
            screenshotRequested = false;
            dumpScreenshot();
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
    }
}

void uiStart() {
    canvas = new GFXcanvas1(EPD_3IN97_WIDTH, EPD_3IN97_HEIGHT);
    canvas->setRotation(SCREEN_ROTATION);
    loopDayMoments = (int *)ps_malloc(MAX_MOMENTS * sizeof(int));
    displayDayMoments = (int *)ps_malloc(MAX_MOMENTS * sizeof(int));
    render(currentView());
    pushFull();
    xTaskCreatePinnedToCore(displayTask, "display", 8192, NULL, 1, &displayTaskHandle, 1);
}

// --- Input ---

static const char *eventName(ButtonEvent event) {
    switch (event) {
    case BTN_UP: return "up";
    case BTN_DOWN: return "down";
    case BTN_UP_RELEASE: return "up released";
    case BTN_DOWN_RELEASE: return "down released";
    case BTN_CLICK: return "click";
    case BTN_LONG_CLICK: return "long click";
    case BTN_POWER: return "power";
    }
    return "?";
}

static void openOverlay(Overlay which, int choice) {
    overlay = which;
    overlayChoice = choice;
    overlaySince = millis();
}

static void showScreen(Screen which, int row) {
    screen = which;
    selected = row;
}

static void openCalendar() {
    const DayIndex &index = daysWithMoments(loopDays);
    if (index.count > 0 && findDay(index, calendarDay) < 0) calendarDay = index.days[index.count - 1].key; // newest
    showScreen(SCREEN_CALENDAR, index.count > 0 ? 0 : ROW_BACK);
}

// Play or stop the moment at list position `index`
static void togglePlay(int index) {
    if (audioState() == AUDIO_PLAYING && audioPlayingIndex() == index) audioStopPlayback();
    else audioPlay(index);
}

static void openMenu(int index) {
    Moment moment;
    if (!momentGet(index, moment)) return;
    if (audioState() == AUDIO_PLAYING) audioStopPlayback();
    menuMoment = moment.number;
    openOverlay(OVERLAY_MENU, moment.tag != TAG_NONE ? moment.tag - TAG_HUMAN : 0);
}

static void handleOverlay(ButtonEvent event) {
    int choices = overlay == OVERLAY_MENU ? MENU_ITEMS : overlay == OVERLAY_GAIN ? GAIN_CHOICE_COUNT : 2;
    switch (event) {
    case BTN_UP:
        overlayChoice = (overlayChoice + choices - 1) % choices;
        overlaySince = millis();
        break;
    case BTN_DOWN:
        overlayChoice = (overlayChoice + 1) % choices;
        overlaySince = millis();
        break;
    case BTN_CLICK: {
        int index = momentIndexByNumber(menuMoment);
        if (overlay == OVERLAY_MENU) {
            if (overlayChoice < MENU_DELETE) {
                Moment moment;
                int tag = TAG_HUMAN + overlayChoice;
                if (momentGet(index, moment)) audioSetTag(index, (MomentTag)(moment.tag == tag ? TAG_NONE : tag));
                overlay = OVERLAY_NONE;
            } else if (overlayChoice == MENU_DELETE) {
                openOverlay(OVERLAY_CONFIRM_DELETE, 0);
            } else {
                overlay = OVERLAY_NONE;
            }
        } else if (overlay == OVERLAY_CONFIRM_DELETE) {
            if (overlayChoice == 1 && index >= 0) audioDelete(index);
            overlay = OVERLAY_NONE;
        } else if (overlay == OVERLAY_CONFIRM_FORMAT) {
            if (overlayChoice == 1) audioFormatCard();
            overlay = OVERLAY_NONE;
        } else if (overlay == OVERLAY_GAIN) {
            audioSetMicGain(GAIN_CHOICES[overlayChoice]);
            overlay = OVERLAY_NONE;
        }
        break;
    }
    default:
        break;
    }
}

static void handleList(ButtonEvent event) {
    switch (event) {
    case BTN_UP:
        if (selected > ROW_SETTINGS) selected = selected - 1;
        break;
    case BTN_DOWN:
        if (selected < momentCount()) selected = selected + 1;
        break;
    case BTN_CLICK:
        if (selected == ROW_SETTINGS) showScreen(SCREEN_SETTINGS, 0);
        else if (selected == ROW_CALENDAR) openCalendar();
        else if (selected == 0) audioStartRecording();
        else togglePlay(selected - 1);
        break;
    case BTN_LONG_CLICK:
        if (selected > 0) openMenu(selected - 1);
        break;
    default:
        break;
    }
}

static void handleDay(ButtonEvent event) {
    int n = momentsOnDay(calendarDay, loopDayMoments, MAX_MOMENTS);
    switch (event) {
    case BTN_UP:
        if (selected > ROW_BACK) selected = selected - 1;
        break;
    case BTN_DOWN:
        if (selected < n - 1) selected = selected + 1;
        break;
    case BTN_CLICK:
        if (selected == ROW_BACK) showScreen(SCREEN_CALENDAR, 0);
        else if (selected < n) togglePlay(loopDayMoments[selected]);
        break;
    case BTN_LONG_CLICK:
        if (selected >= 0 && selected < n) openMenu(loopDayMoments[selected]);
        break;
    default:
        break;
    }
}

static void handleCalendar(ButtonEvent event) {
    const DayIndex &index = daysWithMoments(loopDays);
    int d = findDay(index, calendarDay);
    switch (event) {
    case BTN_UP: // earlier day
        if (selected == ROW_BACK) break;
        if (d > 0) calendarDay = index.days[d - 1].key;
        else selected = ROW_BACK;
        break;
    case BTN_DOWN: // later day
        if (selected == ROW_BACK) {
            if (index.count > 0) selected = 0;
        } else if (d >= 0 && d + 1 < index.count) {
            calendarDay = index.days[d + 1].key;
        }
        break;
    case BTN_CLICK:
        if (selected == ROW_BACK) showScreen(SCREEN_LIST, ROW_CALENDAR);
        else if (d >= 0) showScreen(SCREEN_DAY, 0);
        break;
    case BTN_LONG_CLICK:
        showScreen(SCREEN_LIST, ROW_CALENDAR);
        break;
    default:
        break;
    }
}

static void handleSettings(ButtonEvent event) {
    switch (event) {
    case BTN_UP:
        if (selected > ROW_BACK) selected = selected - 1;
        break;
    case BTN_DOWN:
        if (selected < SET_ROWS - 1) selected = selected + 1;
        break;
    case BTN_LONG_CLICK:
        showScreen(SCREEN_LIST, ROW_SETTINGS);
        break;
    case BTN_CLICK:
        switch (selected) {
        case ROW_BACK:
            showScreen(SCREEN_LIST, ROW_SETTINGS);
            break;
        case SET_SYNC:
            clockSyncNow();
            break;
        case SET_GAIN: {
            int current = 0;
            for (int i = 0; i < GAIN_CHOICE_COUNT; i++) {
                if (GAIN_CHOICES[i] == audioMicGainSetting()) current = i;
            }
            openOverlay(OVERLAY_GAIN, current);
            break;
        }
        case SET_TRANSFER:
            if (clockSyncing()) break; // it has Wi-Fi for now
            transferStart();
            showScreen(SCREEN_TRANSFER, 0);
            break;
        case SET_CARD_READER:
            if (!audioUsingSdCard() || !usbPowered()) break;
            audioSuspend(); // the audio task lets go of the card
            if (cardReaderStart()) showScreen(SCREEN_CARD_READER, 0);
            break;
        case SET_FORMAT:
            if (audioUsingSdCard() && audioState() != AUDIO_FORMATTING) openOverlay(OVERLAY_CONFIRM_FORMAT, 0);
            break;
        }
        break;
    default:
        break;
    }
}

static void handleEvent(ButtonEvent event);

void uiHandleEvent(ButtonEvent event) {
    handleEvent(event);
    if (displayTaskHandle) xTaskNotifyGive(displayTaskHandle);
}

static void handleEvent(ButtonEvent event) {
    AudioState audio = audioState();
    Serial.printf("[ui] %lu ms: %s (screen %d, selected %d, audio %d, overlay %d)\n",
                  millis(), eventName(event), (int)screen, (int)selected, (int)audio, (int)overlay);
    lastActivity = millis();

    if (screen == SCREEN_CARD_READER) {
        if (event == BTN_CLICK) cardReaderLeave(); // restarts
        return; // and no sleep: light sleep would drop the USB drive
    }
    if (event == BTN_POWER) {
        if (screen == SCREEN_TRANSFER) {
            transferStop();
            showScreen(SCREEN_SETTINGS, SET_TRANSFER);
        }
        overlay = OVERLAY_NONE;
        sleepRequested = true;
        return;
    }
    if (event == BTN_UP_RELEASE || event == BTN_DOWN_RELEASE) {
        audioSpeedHold(0);
        return;
    }
    if (overlay != OVERLAY_NONE) {
        handleOverlay(event);
        return;
    }
    if (audio == AUDIO_RECORDING || audio == AUDIO_SAVING) {
        if (event == BTN_CLICK) audioStopRecording();
        return;
    }
    if (audio == AUDIO_FORMATTING) return;
    // Up and down are the tape transport while a moment plays
    if (audio == AUDIO_PLAYING && (event == BTN_UP || event == BTN_DOWN) &&
        (screen == SCREEN_LIST || screen == SCREEN_DAY)) {
        audioSpeedHold(event == BTN_UP ? 1 : -1);
        return;
    }

    switch (screen) {
    case SCREEN_LIST: handleList(event); break;
    case SCREEN_DAY: handleDay(event); break;
    case SCREEN_CALENDAR: handleCalendar(event); break;
    case SCREEN_SETTINGS: handleSettings(event); break;
    case SCREEN_TRANSFER:
        if (event == BTN_CLICK) {
            transferStop();
            showScreen(SCREEN_SETTINGS, SET_TRANSFER);
        }
        break;
    }
}

void uiTick() {
    int count = momentCount();
    if (count > lastMomentCount && screen == SCREEN_LIST) selected = 1; // a recording just finished: select it so a click plays it
    lastMomentCount = count;
    if (screen == SCREEN_LIST && selected > count) selected = count;
    static uint32_t checkedVersion = UINT32_MAX;
    if (screen == SCREEN_DAY && momentListVersion() != checkedVersion) {
        checkedVersion = momentListVersion(); // a moment of the day may have been deleted
        int n = momentsOnDay(calendarDay, loopDayMoments, MAX_MOMENTS);
        if (n == 0) openCalendar();
        else if (selected >= n) selected = n - 1;
    }
    if (screen == SCREEN_CALENDAR && selected == 0) {
        const DayIndex &index = daysWithMoments(loopDays);
        if (index.count == 0) selected = ROW_BACK;
        else if (findDay(index, calendarDay) < 0) calendarDay = index.days[index.count - 1].key;
    }
    if (screen == SCREEN_TRANSFER && transferInfo().state == TRANSFER_OFF) {
        showScreen(SCREEN_SETTINGS, SET_TRANSFER); // stopped after 10 minutes unused
    }

    if (overlay != OVERLAY_NONE) {
        uint32_t timeout = overlay == OVERLAY_MENU ? MENU_TIMEOUT_MS : CONFIRM_TIMEOUT_MS;
        if (millis() - overlaySince > timeout) overlay = OVERLAY_NONE;
        bool aboutMoment = overlay == OVERLAY_MENU || overlay == OVERLAY_CONFIRM_DELETE;
        if (aboutMoment && momentIndexByNumber(menuMoment) < 0) overlay = OVERLAY_NONE; // deleted meanwhile
    }
    int result = audioFormatResult();
    if (result != lastFormatResult) {
        lastFormatResult = result;
        formatResultAt = millis();
    }

    // Wi-Fi and the USB drive need the chip awake
    bool busy = audioState() != AUDIO_IDLE || screen == SCREEN_TRANSFER || screen == SCREEN_CARD_READER;
    if (busy) lastActivity = millis();
    if (millis() - lastActivity > AUTO_SLEEP_MS) {
        Serial.printf("[ui] idle for %lu s, going to sleep\n", AUTO_SLEEP_MS / 1000);
        lastActivity = millis();
        overlay = OVERLAY_NONE;
        sleepRequested = true;
    }
}

void uiRequestScreenshot() { screenshotRequested = true; }

// Signed, so it holds across millis() wrapping
static bool after(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }

bool uiReadyToNap(uint32_t since) {
    if (sleeping || overlay != OVERLAY_NONE || audioState() != AUDIO_IDLE) return false;
    if (screen == SCREEN_TRANSFER || screen == SCREEN_CARD_READER) return false;
    uint32_t quiet = quietSince;
    // The display task must have looked at the screen state after the last change
    return quiet != 0 && after(quiet, since) && after(quiet, lastActivity);
}

uint32_t uiMsUntilNextChange() {
    uint32_t now = millis();
    uint32_t idle = now - lastActivity;
    uint32_t ms = idle < AUTO_SLEEP_MS ? AUTO_SLEEP_MS - idle : 0;

    timeval tv;
    gettimeofday(&tv, nullptr);
    uint32_t toMinute = 60000 - (tv.tv_sec % 60) * 1000 - tv.tv_usec / 1000 + 5; // just past the minute
    ms = min(ms, toMinute);

    // Sync and format results go away after RESULT_MS
    uint32_t since;
    ClockSync sync = clockSyncState(since);
    if (sync != CLOCK_SYNC_NONE && sync != CLOCK_SYNCING && now - since <= RESULT_MS) {
        ms = min(ms, RESULT_MS - (now - since) + 5);
    }
    if (lastFormatResult != 0 && now - formatResultAt <= RESULT_MS) {
        ms = min(ms, RESULT_MS - (now - formatResultAt) + 5);
    }
    return ms;
}

bool uiTakeSleepRequest() {
    bool requested = sleepRequested;
    sleepRequested = false;
    return requested;
}

void uiShowSleepScreen(bool on) {
    if (on) quoteIndex = esp_random() % QUOTE_COUNT;
    sleeping = on;
    if (!on) {
        lastActivity = millis(); // a full idle period again after waking
        return; // waking happens in the background
    }
    uint32_t start = millis();
    while (!panelAsleep && millis() - start < SLEEP_SCREEN_TIMEOUT_MS) delay(20);
}
