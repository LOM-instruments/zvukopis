#include "ui.h"

#include <Adafruit_GFX.h>
#include <Arduino.h>
#include <mbedtls/base64.h>
#include <qrcode.h>
#include <sys/time.h>

#include "EPD_3in97.h"
#include "audio.h"
#include "board_power.h"
#include "cardreader.h"
#include "clock.h"
#include "config.h"
#include "fonts.h"
#include "networks.h"
#include "quotes.h"
#include "spectrum.h"
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
static const int HEADER_LINE_Y = 90;
static const int LIST_TOP = 106;
static const int ROW_H = 96;
static const int VISIBLE_ROWS = 6;
static const int FOOTER_LINE_Y = 690; // room for three lines of hints below
static const int WAVE_X = MARGIN + 44;
static const int WAVE_PITCH = 5; // bar width 3 + gap 2
static const int WAVE_HALF_H = 15;

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
    SCREEN_SCHEDULE,    // the scheduled recordings
    SCREEN_SPECTRUM,    // frequency analysis of one moment
};

enum Overlay {
    OVERLAY_NONE,
    OVERLAY_MENU,           // tag / delete, on a long press of a moment
    OVERLAY_CONFIRM_DELETE, // Cancel / Delete
    OVERLAY_CONFIRM_FORMAT, // Cancel / Format
    OVERLAY_GAIN,           // mic gain choices
    OVERLAY_CONFIRM_UNSCHEDULE, // Cancel / Remove a scheduled recording
};

// Header buttons have negative positions: up from the first row reaches them
static const int ROW_BACK = -1;     // sub-screens
static const int ROW_CALENDAR = -1; // main list

enum SettingsRow { SET_GAIN, SET_SCHEDULE, SET_WEB, SET_HOTSPOT, SET_SYNC, SET_CARD_READER, SET_FORMAT, SET_ROWS };
static const int GAIN_CHOICES[] = {GAIN_AUTO, 0, 6, 12, 18, 24, 30, 36, 42, 48, 54};
static const int GAIN_CHOICE_COUNT = sizeof(GAIN_CHOICES) / sizeof(GAIN_CHOICES[0]);
static const int MENU_ANALYSIS = TAG_COUNT - 1; // after the five tags
static const int MENU_DELETE = TAG_COUNT;
static const int MENU_CANCEL = TAG_COUNT + 1;
static const int MENU_ITEMS = TAG_COUNT + 2;

// Frequency analysis plot; labels go left of and below it
static const int PLOT_X = MARGIN + 46, PLOT_Y = 116;
static const int PLOT_W = SW - MARGIN - PLOT_X, PLOT_H = 436;


// UI state: written by loop(), read by the display task
static volatile int screen = SCREEN_LIST;
static volatile int selected = 0;    // list: row 0 = New recording, row n = moment n-1; day: row n = its moment n
static volatile int calendarDay = 0; // yyyymmdd of the day chosen in the calendar
static volatile int overlay = OVERLAY_NONE;
static volatile int overlayChoice = 0;
static uint32_t overlaySince = 0;
static int menuMoment = 0; // number of the moment the menu is for
static int transferRow = SET_WEB; // settings row the web interface was started from
static int64_t unscheduleStart = 0;     // the scheduled recording the remove dialog is for
static Screen scheduleFrom = SCREEN_LIST; // where Back on the schedule screen goes
static Screen menuFrom = SCREEN_LIST;     // where closing the menu (Settings) returns to
static int menuFromSelected = 0;
static volatile int spectrumMoment = 0; // number of the moment being analysed
enum SpectrumView { VIEW_SPECTROGRAM, VIEW_FFT };
static volatile int spectrumView = VIEW_SPECTROGRAM;
static Screen spectrumFrom = SCREEN_LIST;
static int spectrumFromSelected = 0;
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

// Main list rows: New recording, the next scheduled recording (when there
// are any), then the moments
static int firstMomentRow(bool scheduled) { return scheduled ? 2 : 1; }
static const int ROW_NEXT_SCHEDULED = 1;

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
    int cardUnreadable; // a card is in, but its file system can't be read
    int clockMinute; // -1 while the clock isn't set; changes once a minute
    int clockSync;   // ClockSync, NONE once a result has been up for RESULT_MS
    int formatStatus; // 0, 1 done, -1 failed (for RESULT_MS), 2 formatting
    int transferState;
    uint32_t transferRequests;
    int cardEjected;
    int gainSetting;
    int gainNow;
    uint32_t configVersion; // categories or schedule edited on the web interface
    int scheduleCount;
    int spectrumMoment;
    int spectrumReady;
    int spectrumProgress; // in steps of 10%, so the screen isn't redrawn for every column
    int spectrumView;
    int playTicks; // playback position in 1/8 s: changes continuously, so the screen refreshes back to back
    int nextScheduleMinute; // start of the next scheduled recording, -1 if none
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
    v.cardUnreadable = storageCardState() == SD_UNREADABLE;
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
    v.configVersion = configVersion();
    Schedule next;
    int nextIndex;
    v.nextScheduleMinute = clockValid() && scheduleNext(time(nullptr), next, nextIndex) ? (int)(next.start / 60) : -1;
    v.scheduleCount = scheduleCount();
    v.spectrumMoment = spectrumMoment;
    v.spectrumReady = screen == SCREEN_SPECTRUM && spectrumResult(spectrumMoment) != nullptr;
    v.spectrumProgress = screen == SCREEN_SPECTRUM && !v.spectrumReady ? spectrumProgress(spectrumMoment) / 10 * 10 : 0;
    v.spectrumView = spectrumView;
    if (v.audio == AUDIO_PLAYING) v.playTicks = (int)(audioPlaybackSeconds() * 8);
    return v;
}

// --- Drawing helpers ---

static void formatTime(char *buf, size_t len, int seconds) {
    snprintf(buf, len, "%d:%02d", seconds / 60, seconds % 60);
}

// "1h 23m", "1m 15s", "40s": units spelt out, so it can't be read as a clock time
static void formatTimeLeft(char *buf, size_t len, int seconds) {
    if (seconds >= 3600) snprintf(buf, len, "%dh %02dm", seconds / 3600, seconds / 60 % 60);
    else if (seconds >= 60) snprintf(buf, len, "%dm %02ds", seconds / 60, seconds % 60);
    else snprintf(buf, len, "%ds", seconds);
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

// Word-wrapped, centred on x or (centred false) starting at it; returns the
// baseline after the last line. With draw false it only measures.
static int drawWrapped(int x, int y, int maxWidth, int lineHeight, const GFXfont *font, const char *text,
                       bool draw = true, bool centred = true) {
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
        if (draw && centred) drawTextCentered(x, y, font, line, INK);
        else if (draw) drawText(x, y, font, line, INK);
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

// The rocker pressed in: a dot for a short press, a pill for holding it
static void iconPress(int cx, int cy) { canvas->fillCircle(cx, cy, 6, INK); }
static void iconHold(int x, int cy) { canvas->fillRoundRect(x, cy - 6, 26, 13, 6, INK); }

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

// Bars of a spectrum, for frequency analysis
static void iconBars(int cx, int cy, int r, uint16_t c) {
    const int heights[5] = {5, 10, 7, 12, 4}; // of 12
    for (int i = 0; i < 5; i++) {
        int h = heights[i] * r * 2 / 12;
        canvas->fillRect(cx - r + i * r * 2 / 5, cy + r - h, max(2, r * 2 / 5 - 2), h, c);
    }
}

static void iconClock(int cx, int cy, int r, uint16_t c) {
    drawArc(cx, cy, r, 0, 360, max(2, r / 5), c);
    canvas->fillRect(cx - 1, cy - r * 6 / 10, 3, r * 6 / 10 + 1, c); // minute hand, at twelve
    canvas->fillRect(cx, cy - 1, r * 45 / 100, 3, c);                // hour hand, at three
}

// "in 3 h 12 min", for something `seconds` from now
static void formatIn(char *buf, size_t len, int64_t seconds) {
    if (seconds <= 0) snprintf(buf, len, "now");
    else if (seconds < 60) snprintf(buf, len, "in under a minute");
    else if (seconds < 3600) snprintf(buf, len, "in %d min", (int)(seconds / 60));
    else if (seconds < 48 * 3600) snprintf(buf, len, "in %d h %d min", (int)(seconds / 3600), (int)(seconds / 60 % 60));
    else snprintf(buf, len, "in %d days", (int)(seconds / 86400));
}

static void iconWifi(int cx, int cy) {
    for (int r = 14; r <= 42; r += 14) drawArc(cx, cy, r, 225, 315, 5, INK);
    canvas->fillCircle(cx, cy - 2, 5, INK);
}

// A category's icon (CategoryIcon); r is about half the icon's size
static void iconCategory(int icon, int cx, int cy, int r, uint16_t c) {
    uint16_t hole = c == INK ? PAPER : INK;
    int thick = max(2, r / 6);
    switch (icon) {
    case ICON_PERSON: // head and shoulders
        canvas->fillCircle(cx, cy - r * 4 / 10, r * 4 / 10, c);
        canvas->fillRoundRect(cx - r * 8 / 10, cy + r / 10, r * 16 / 10, r * 9 / 10, r * 4 / 10, c);
        break;
    case ICON_TREE:
        canvas->fillTriangle(cx, cy - r, cx - r * 8 / 10, cy + r * 4 / 10, cx + r * 8 / 10, cy + r * 4 / 10, c);
        canvas->fillRect(cx - max(1, r / 6), cy + r * 4 / 10, max(2, r / 3), r * 6 / 10, c);
        break;
    case ICON_SMILEY:
        drawArc(cx, cy, r, 0, 360, thick, c);
        canvas->fillCircle(cx - r * 35 / 100, cy - r * 3 / 10, max(1, r / 7), c);
        canvas->fillCircle(cx + r * 35 / 100, cy - r * 3 / 10, max(1, r / 7), c);
        drawArc(cx, cy, r * 55 / 100, 25, 155, max(2, r / 7), c);
        break;
    case ICON_PALETTE:
        canvas->fillCircle(cx, cy, r, c);
        canvas->fillCircle(cx + r * 4 / 10, cy + r * 4 / 10, r * 25 / 100, hole);
        canvas->fillCircle(cx - r * 45 / 100, cy - r * 15 / 100, max(1, r / 6), hole);
        canvas->fillCircle(cx - r * 10 / 100, cy - r * 50 / 100, max(1, r / 6), hole);
        canvas->fillCircle(cx + r * 35 / 100, cy - r * 35 / 100, max(1, r / 6), hole);
        break;
    case ICON_WAVE: // two sine periods
        for (int x = -r; x <= r; x++) {
            int y = (int)roundf(sinf((float)x / r * 2 * (float)M_PI) * r * 0.45f);
            canvas->fillRect(cx + x, cy + y - thick / 2, 1, thick, c);
        }
        break;
    case ICON_NOTE: // an eighth note
        canvas->fillCircle(cx - r * 3 / 10, cy + r * 55 / 100, r * 38 / 100, c);
        canvas->fillRect(cx - r * 3 / 10 + r * 38 / 100 - thick, cy - r, thick, r * 16 / 10, c);
        canvas->fillTriangle(cx + r * 8 / 100, cy - r, cx + r * 8 / 100, cy - r * 4 / 10, cx + r * 7 / 10, cy - r * 3 / 10, c);
        break;
    case ICON_HEART:
        canvas->fillCircle(cx - r * 45 / 100, cy - r * 25 / 100, r * 48 / 100, c);
        canvas->fillCircle(cx + r * 45 / 100, cy - r * 25 / 100, r * 48 / 100, c);
        canvas->fillTriangle(cx - r * 92 / 100, cy - r * 10 / 100, cx + r * 92 / 100, cy - r * 10 / 100, cx, cy + r, c);
        break;
    case ICON_DROP:
        canvas->fillCircle(cx, cy + r * 3 / 10, r * 6 / 10, c);
        canvas->fillTriangle(cx, cy - r, cx - r * 57 / 100, cy + r * 15 / 100, cx + r * 57 / 100, cy + r * 15 / 100, c);
        break;
    case ICON_SUN:
        canvas->fillCircle(cx, cy, r * 45 / 100, c);
        for (int i = 0; i < 8; i++) {
            float a = i * (float)M_PI / 4;
            canvas->drawLine(cx + (int)(cosf(a) * r * 0.65f), cy + (int)(sinf(a) * r * 0.65f),
                             cx + (int)(cosf(a) * r), cy + (int)(sinf(a) * r), c);
        }
        break;
    default:
        iconStar(cx, cy, r, c);
        break;
    }
}

// The icon of the category a moment is tagged with
static void iconTag(int tag, int cx, int cy, int r, uint16_t c) {
    iconCategory(configCategory(tag).icon, cx, cy, r, c);
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

enum HintIcon { HINT_UPDOWN, HINT_CLICK, HINT_HOLD_CLICK, HINT_PWR, HINT_BOOT };

// A key name in a rounded box, like a keycap. Returns its width.
static int drawKeycap(int x, int cy, const char *name) {
    int w = textWidth(&Text17, name) + 12;
    canvas->drawRoundRect(x, cy - 12, w, 24, 5, INK);
    drawText(x + 6, cy + 6, &Text17, name, INK);
    return w;
}

// Returns the hint's width
static int drawHint(int x, int cy, HintIcon icon, const char *label) {
    int start = x;
    switch (icon) {
    case HINT_UPDOWN:
        iconUpDown(x + 6, cy, INK);
        x += 20;
        break;
    case HINT_CLICK:
        iconPress(x + 10, cy);
        x += 26;
        break;
    case HINT_HOLD_CLICK:
        iconHold(x + 4, cy);
        x += 40;
        break;
    case HINT_PWR:
        x += drawKeycap(x, cy, "PWR") + 8;
        break;
    case HINT_BOOT:
        x += drawKeycap(x, cy, "BOOT") + 8;
        break;
    }
    drawText(x, cy + 7, &Text22, label, INK);
    return x - start + textWidth(&Text22, label);
}

// A header button: icon in a rounded box, filled when selected
static void drawHeaderButton(int x, int cy, int size, bool isSelected, void (*icon)(int, int, uint16_t, uint16_t)) {
    int y = cy - size / 2;
    uint16_t fg = isSelected ? PAPER : INK, bg = isSelected ? INK : PAPER;
    if (isSelected) canvas->fillRoundRect(x, y, size, size, 8, INK);
    else canvas->drawRoundRect(x, y, size, size, 8, INK);
    icon(x + size / 2, cy, fg, bg);
}

// --- Screen sections ---

// SD card, USB and battery, right-aligned at `right`; returns the left edge
// Header status text: one size and weight, on the title's baseline, with the
// icons centred on its lowercase letters
static const GFXfont *const STATUS_FONT = &Text22;
static const int TITLE_BASELINE = 40; // first header line; the second is SECOND_BASELINE
static const int SECOND_BASELINE = 74;
static const int HEADER_BUTTON = 34;  // calendar and Back buttons

static int drawStatus(int right, int baseline, const ViewState &v) {
    int x = right, cy = baseline - 8;
    char buf[8];
    if (v.battery >= 0) {
        x = iconBattery(x, cy, v.battery) - 8;
        snprintf(buf, sizeof(buf), "%d%%", v.battery);
        drawTextRight(x, baseline, STATUS_FONT, buf, INK);
        x -= textWidth(STATUS_FONT, buf) + 10;
    }
    if (v.usb) {
        iconBolt(x - 6, cy);
        x -= 22;
    }
    if (v.sdCard) x = iconSdCard(x, cy) - 10;
    return x;
}

// "27 Oct  18:23" in the status font
static void formatDateTime(char *buf, size_t len, const ViewState &v) {
    buf[0] = 0;
    if (v.clockMinute < 0) return;
    char date[16], hhmm[8];
    time_t now = (time_t)v.clockMinute * 60;
    formatLocal(date, sizeof(date), "%e %b", now);
    formatLocal(hhmm, sizeof(hhmm), "%H:%M", now);
    snprintf(buf, len, "%s  %s", date, hhmm);
}

static void drawDateTime(const ViewState &v) {
    char buf[24];
    formatDateTime(buf, sizeof(buf), v);
    drawTextRight(SW - MARGIN, SECOND_BASELINE, STATUS_FONT, buf, INK);
}

// First line: name, date and time, battery. Second: moments, time left, and
// the calendar button.
static void drawMainHeader(const ViewState &v) {
    drawText(MARGIN, TITLE_BASELINE, &Bold22, "zvukopis", INK);
    int statusLeft = drawStatus(SW - MARGIN, TITLE_BASELINE, v);
    char when[24];
    formatDateTime(when, sizeof(when), v);
    int titleRight = MARGIN + textWidth(&Bold22, "zvukopis");
    drawTextCentered((titleRight + statusLeft) / 2, TITLE_BASELINE, STATUS_FONT, when, INK);

    char buf[40];
    // "2 moments  •  3:04 left"
    if (v.momentCount == 1) snprintf(buf, sizeof(buf), "1 moment");
    else snprintf(buf, sizeof(buf), "%d moments", v.momentCount);
    drawText(MARGIN, SECOND_BASELINE, &Text22, buf, INK);
    int x = MARGIN + textWidth(&Text22, buf) + 14;
    canvas->fillCircle(x, SECOND_BASELINE - 6, 2, INK);
    if (v.secondsLeft < 1) {
        snprintf(buf, sizeof(buf), v.sdCard ? "card full" : "memory full");
    } else {
        char left[16];
        formatTimeLeft(left, sizeof(left), v.secondsLeft);
        snprintf(buf, sizeof(buf), "%s to record", left);
    }
    drawText(x + 14, SECOND_BASELINE, &Text22, buf, INK);
    // The calendar has nothing to show until there are moments
    if (v.momentCount > 0 || v.selected == ROW_CALENDAR) {
        drawHeaderButton(SW - MARGIN - HEADER_BUTTON, SECOND_BASELINE - 7, HEADER_BUTTON, v.selected == ROW_CALENDAR, iconCalendar);
    }
    canvas->fillRect(MARGIN, HEADER_LINE_Y, SW - 2 * MARGIN, 2, INK);
}

// Back button and title; a subtitle and the clock below
static void drawSubHeader(const ViewState &v, const char *title, const char *subtitle, bool canGoBack = true) {
    int x = MARGIN;
    if (canGoBack) {
        bool isSelected = v.selected == ROW_BACK;
        const int w = 86, y = TITLE_BASELINE - 8 - HEADER_BUTTON / 2;
        uint16_t fg = isSelected ? PAPER : INK;
        if (isSelected) canvas->fillRoundRect(x, y, w, HEADER_BUTTON, 8, INK);
        else canvas->drawRoundRect(x, y, w, HEADER_BUTTON, 8, INK);
        canvas->fillTriangle(x + 11, y + 17, x + 20, y + 9, x + 20, y + 25, fg);
        drawText(x + 27, TITLE_BASELINE, &Text22, "Back", fg);
        x += w + 12;
    }
    drawText(x, TITLE_BASELINE, &Bold22, title, INK);
    drawStatus(SW - MARGIN, TITLE_BASELINE, v);
    if (subtitle) drawText(MARGIN, SECOND_BASELINE, &Text22, subtitle, INK);
    drawDateTime(v);
    canvas->fillRect(MARGIN, HEADER_LINE_Y, SW - 2 * MARGIN, 2, INK);
}

static void drawNewRecordingRow(int y, uint16_t fg, bool isSelected, const ViewState &v) {
    int cy = y + ROW_H / 2;
    if (v.momentCount == 0) {
        // First time: the switch sits beside this row, so the row points at it
        canvas->fillTriangle(MARGIN - 6, cy, MARGIN + 10, cy - 10, MARGIN + 10, cy + 10, fg);
        canvas->fillRect(MARGIN + 10, cy - 3, 16, 7, fg);
        iconRecord(MARGIN + 56, cy, fg);
        drawText(MARGIN + 86, cy + 8, &Bold22, "Press in to record", fg);
        return;
    }
    iconRecord(MARGIN + 14, cy, fg);
    drawText(MARGIN + 44, cy + 8, &Bold22, "New recording", fg);
    if (isSelected && v.secondsLeft < 1) drawTextRight(SW - MARGIN, cy + 8, &Text22, v.sdCard ? "card full" : "memory full", fg);
}

// When the next scheduled recording is, and how long it lasts
static void drawNextScheduledRow(int y, uint16_t fg, const ViewState &v) {
    int lineY = y + 40;
    iconClock(MARGIN + 14, lineY - 8, 11, fg);
    drawText(MARGIN + 44, lineY, &Bold22, "Next recording", fg);
    Schedule next;
    int index;
    char buf[48];
    if (v.nextScheduleMinute < 0 || !scheduleNext(time(nullptr), next, index)) {
        drawText(MARGIN + 44, y + 70, &Text17, v.clockMinute < 0 ? "Set the clock for these to run" : "", fg);
        return;
    }
    char in[24];
    formatIn(in, sizeof(in), next.start - (int64_t)v.clockMinute * 60);
    drawTextRight(SW - MARGIN, lineY, &Text22, in, fg);
    char when[24], length[12];
    formatLocal(when, sizeof(when), "%a %e %b, %H:%M", (time_t)next.start);
    formatTime(length, sizeof(length), next.seconds);
    snprintf(buf, sizeof(buf), "%s  -  %s%s", when, length, next.daily ? "  -  every day" : "");
    drawText(MARGIN + 44, y + 70, &Text17, buf, fg);
    if (v.scheduleCount > 1) {
        snprintf(buf, sizeof(buf), "+%d more", v.scheduleCount - 1);
        drawTextRight(SW - MARGIN, y + 70, &Text17, buf, fg);
    }
}

// Two lines: name, date and tag, duration; then a waveform. While playing,
// the part not yet played is drawn as hairlines, with a playhead.
static void drawMomentRow(int index, int y, uint16_t fg, const ViewState &v, bool showDate) {
    Moment moment;
    if (!momentGet(index, moment)) return;
    bool playing = v.audio == AUDIO_PLAYING && v.playingIndex == index;

    int lineY = y + 40;
    if (playing) iconStop(MARGIN + 14, lineY - 8, fg);
    else iconPlay(MARGIN + 14, lineY - 8, fg);

    char buf[32];
    snprintf(buf, sizeof(buf), "Moment %d", moment.number);
    drawText(MARGIN + 44, lineY, &Bold22, buf, fg);
    int x = MARGIN + 44 + textWidth(&Bold22, buf) + 12;
    if (moment.recordedAt && !playing) { // while playing, the speed takes its place
        formatLocal(buf, sizeof(buf), showDate ? "%e %b %H:%M" : "%H:%M:%S", moment.recordedAt);
        drawText(x, lineY, &Text22, buf, fg);
        x += textWidth(&Text22, buf) + 14;
    }
    if (moment.tag != TAG_NONE) iconTag(moment.tag, x + 11, lineY - 8, 11, fg); // reads as part of the description

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
    drawTextRight(SW - MARGIN, lineY, &Mono22, buf, fg);

    int waveY = y + 68;
    int progressBin = WAVEFORM_BINS;
    if (playing && moment.length > 0) {
        progressBin = constrain((int)(v.playTicks / 8.0f * SAMPLE_RATE / moment.length * WAVEFORM_BINS), 0, WAVEFORM_BINS);
    }
    for (int b = 0; b < WAVEFORM_BINS; b++) {
        int bx = WAVE_X + b * WAVE_PITCH;
        int h = 1 + moment.waveform[b] * WAVE_HALF_H / 255;
        if (b < progressBin) canvas->fillRect(bx, waveY - h, 3, 2 * h + 1, fg); // played: full bar
        else canvas->fillRect(bx + 1, waveY - h, 1, 2 * h + 1, fg);             // still to come: hairline
    }
    if (playing) { // playhead, like the one on the analysis screen
        int px = WAVE_X + progressBin * WAVE_PITCH - 1;
        canvas->fillRect(px, waveY - WAVE_HALF_H - 4, 3, 2 * WAVE_HALF_H + 9, fg);
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

        int first = firstMomentRow(v.scheduleCount > 0);
        if (withNewRecording && row == 0) drawNewRecordingRow(y, fg, isSelected, v);
        else if (withNewRecording && row < first) drawNextScheduledRow(y, fg, v);
        else if (withNewRecording) drawMomentRow(row - first, y, fg, v, true);
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
    int first = firstMomentRow(v.scheduleCount > 0);
    drawRows(v, first + v.momentCount, true, nullptr);
    if (v.momentCount == 0) {
        int cx = SW / 2, y = LIST_TOP + first * ROW_H + 110;
        drawTextCentered(cx, y, &Bold22, "No moments yet", INK);
        drawTextCentered(cx, y + 38, &Text22, "A moment is a recording:", INK);
        drawTextCentered(cx, y + 70, &Text22, "the sound of here and now.", INK);
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
        drawTextCentered(cx, 300, &Bold22, "No dated moments yet", INK);
        drawTextCentered(cx, 338, &Text22, "Moments get a date once", INK);
        drawTextCentered(cx, 366, &Text22, "the clock is set", INK);
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
    drawTextCentered(cx, 150, &Bold32, buf, INK);
    // Other months with moments, either side
    if (index.days[0].key / 100 < key / 100) canvas->fillTriangle(MARGIN, 140, MARGIN + 12, 130, MARGIN + 12, 150, INK);
    if (index.days[index.count - 1].key / 100 > key / 100) {
        canvas->fillTriangle(SW - MARGIN, 140, SW - MARGIN - 12, 130, SW - MARGIN - 12, 150, INK);
    }

    const int cellW = (SW - 2 * MARGIN) / 7, cellH = 62, gridTop = 196;
    const char *weekdays[7] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
    for (int c = 0; c < 7; c++) {
        drawTextCentered(MARGIN + c * cellW + cellW / 2, 184, &Text17, weekdays[c], INK);
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
        drawTextCentered(dayCx, y + 32, found >= 0 ? &Bold22 : &Text22, buf, fg);
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
    drawText(MARGIN, y, &Bold22, buf, INK);
    char total2[12];
    formatTime(total2, sizeof(total2), day.seconds);
    snprintf(buf, sizeof(buf), "%d moment%s, %s", day.count, day.count == 1 ? "" : "s", total2);
    drawText(MARGIN, y + 30, &Text22, buf, INK);
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
            else snprintf(detail, len, "Add a Wi-Fi network on the web interface first");
            enabled = clockSyncAvailable();
            break;
        }
        break;
    case SET_GAIN:
        title = "Mic gain";
        if (v.gainSetting == GAIN_AUTO) snprintf(detail, len, "Auto, now %d dB", v.gainNow);
        else snprintf(detail, len, "%d dB", v.gainSetting);
        break;
    case SET_SCHEDULE:
        title = "Scheduled recordings";
        if (v.nextScheduleMinute >= 0) {
            char when[24];
            formatLocal(when, sizeof(when), "%a %e %b %H:%M", (time_t)v.nextScheduleMinute * 60);
            int n = scheduleCount();
            snprintf(detail, len, "Next %s%s", when, n > 1 ? "  (+ more)" : "");
        } else {
            snprintf(detail, len, "None; set them up on the web interface");
        }
        break;
    case SET_WEB:
        title = "Web interface";
        snprintf(detail, len, "%s", wifiKnowsAny() ? "Moments, schedule, settings, on your Wi-Fi"
                                                  : "Set up Wi-Fi: scan the code it shows");
        break;
    case SET_HOTSPOT:
        title = "Web interface on own hotspot";
        snprintf(detail, len, "zvukopis makes its own Wi-Fi network");
        break;
    case SET_CARD_READER:
        title = "USB card reader";
        enabled = (v.sdCard || v.cardUnreadable) && v.usb;
        snprintf(detail, len, !(v.sdCard || v.cardUnreadable) ? "No SD card"
                              : !v.usb                        ? "Connect USB to a computer first"
                                                              : "Open the SD card on the computer");
        break;
    case SET_FORMAT:
        title = "Format SD card";
        enabled = (v.sdCard || v.cardUnreadable) && v.formatStatus != 2;
        if (v.formatStatus == 2) snprintf(detail, len, "Formatting...");
        else if (v.formatStatus == 1) snprintf(detail, len, "Formatted");
        else if (v.formatStatus == -1) snprintf(detail, len, "Formatting failed");
        else if (v.cardUnreadable) snprintf(detail, len, "The card can't be read: format it to use it");
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
    const int rowH = 72;
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
        drawText(MARGIN, y + 33, enabled ? &Bold22 : &Text22, title, fg);
        drawText(MARGIN, y + 58, &Text17, detail, fg);
    }

    // Card and memory
    int y = LIST_TOP + SET_ROWS * rowH + 40;
    char buf[64], total[16], free[16];
    if (v.sdCard) {
        formatBytes(total, sizeof(total), storageTotalBytes());
        formatBytes(free, sizeof(free), storageFreeBytes());
        iconSdCard(MARGIN + 16, y - 6);
        snprintf(buf, sizeof(buf), "%s card, %s free", total, free);
        drawText(MARGIN + 30, y, &Text22, buf, INK);
    } else {
        drawText(MARGIN, y, &Text17, v.cardUnreadable ? "SD card found, but it can't be read (exFAT?): format it"
                                                   : "No SD card: moments stay in memory, lost on restart", INK);
    }
}

// A QR code, `size` pixels square at x, y (the screen around it is the quiet zone)
static struct {
    int x, y, size;
} qrTarget;

static void drawQrModules(esp_qrcode_handle_t qr) {
    int modules = esp_qrcode_get_size(qr);
    int scale = qrTarget.size / modules;
    int offset = (qrTarget.size - modules * scale) / 2;
    for (int my = 0; my < modules; my++) {
        for (int mx = 0; mx < modules; mx++) {
            if (esp_qrcode_get_module(qr, mx, my)) {
                canvas->fillRect(qrTarget.x + offset + mx * scale, qrTarget.y + offset + my * scale, scale, scale, INK);
            }
        }
    }
}

static void drawQr(const char *text, int x, int y, int size) {
    qrTarget = {x, y, size};
    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func = drawQrModules;
    cfg.qrcode_ecc_level = ESP_QRCODE_ECC_MED;
    esp_qrcode_generate(&cfg, text);
}

// On its own hotspot: a QR code that joins it (phone camera), then the page
// opens by itself. On the usual network: a QR code with the page's address.
static void drawTransfer(const ViewState &v) {
    TransferInfo info = transferInfo();
    drawSubHeader(v, "Web interface", info.state == TRANSFER_READY && info.hotspot ? "zvukopis's own Wi-Fi is on" : "Wi-Fi is on", false);
    int cx = SW / 2, y = 250;
    if (info.state != TRANSFER_READY) {
        iconWifi(cx, y);
        drawTextCentered(cx, y + 70, &Bold32, "Connecting...", INK);
        return;
    }
    const int qrSize = 232, qrY = 132;
    char buf[96];
    if (info.hotspot) {
        snprintf(buf, sizeof(buf), "WIFI:T:WPA;S:%s;P:%s;;", info.network, info.password);
        drawQr(buf, cx - qrSize / 2, qrY, qrSize);
        y = qrY + qrSize + 40;
        drawTextCentered(cx, y, &Bold22, "Scan with a phone camera to join", INK);
        snprintf(buf, sizeof(buf), "or join  %s,  password  %s", info.network, info.password);
        drawTextCentered(cx, y + 32, &Text22, buf, INK);
        drawTextCentered(cx, y + 74, &Text22, "The page opens by itself, or go to", INK);
    } else {
        snprintf(buf, sizeof(buf), "http://%s/", info.address);
        drawQr(buf, cx - qrSize / 2, qrY, qrSize);
        y = qrY + qrSize + 40;
        drawTextCentered(cx, y, &Bold22, "Scan with a phone camera", INK);
        snprintf(buf, sizeof(buf), "or on  %s,  go to", info.network);
        drawTextCentered(cx, y + 32, &Text22, buf, INK);
        drawTextCentered(cx, y + 116, &Text22, "or http://zvukopis.local", INK);
        y -= 42;
    }
    snprintf(buf, sizeof(buf), "http://%s", info.address);
    drawTextCentered(cx, y + 116, &Bold32, buf, INK);
    y += 190;
    if (v.transferRequests > 0) snprintf(buf, sizeof(buf), "%lu request%s served  -  ", v.transferRequests, v.transferRequests == 1 ? "" : "s");
    else buf[0] = 0;
    strlcat(buf, "stops after 10 min unused", sizeof(buf));
    drawTextCentered(cx, min(y, FOOTER_LINE_Y - 16), &Text17, buf, INK);
}

// Every scheduled recording, soonest first
static void drawScheduleScreen(const ViewState &v) {
    char subtitle[32];
    snprintf(subtitle, sizeof(subtitle), "%d recording%s", v.scheduleCount, v.scheduleCount == 1 ? "" : "s");
    drawSubHeader(v, "Schedule", subtitle);
    int cx = SW / 2;
    if (v.scheduleCount == 0) {
        drawTextCentered(cx, 300, &Bold22, "Nothing scheduled", INK);
        drawTextCentered(cx, 338, &Text22, "Add recordings on the web interface:", INK);
        drawTextCentered(cx, 366, &Text22, "Settings, Web interface", INK);
        return;
    }
    const int rowH = 74, visible = (FOOTER_LINE_Y - LIST_TOP - 8) / rowH;
    static int scrollTop = 0;
    int sel = max(0, v.selected);
    if (sel < scrollTop) scrollTop = sel;
    if (sel >= scrollTop + visible) scrollTop = sel - visible + 1;
    scrollTop = constrain(scrollTop, 0, max(0, v.scheduleCount - visible));

    for (int i = 0; i < visible && scrollTop + i < v.scheduleCount; i++) {
        int row = scrollTop + i, y = LIST_TOP + i * rowH;
        Schedule s;
        if (!scheduleGet(row, s)) break;
        bool isSelected = row == v.selected;
        uint16_t fg = isSelected ? PAPER : INK;
        if (isSelected) canvas->fillRoundRect(MARGIN - 12, y + 4, SW - 2 * MARGIN + 24, rowH - 8, 12, INK);
        else if (i > 0 && row - 1 != v.selected) dottedLine(MARGIN, SW - MARGIN, y);

        char when[32], length[12], detail[48];
        formatLocal(when, sizeof(when), s.daily ? "Every day, %H:%M:%S" : "%a %e %b, %H:%M:%S", (time_t)s.start);
        formatTime(length, sizeof(length), s.seconds);
        drawText(MARGIN, y + 33, &Bold22, when, fg);
        drawTextRight(SW - MARGIN, y + 33, &Mono22, length, fg);
        char in[24];
        formatIn(in, sizeof(in), v.clockMinute >= 0 ? s.start - (int64_t)v.clockMinute * 60 : 0);
        if (s.daily) {
            char next[24];
            formatLocal(next, sizeof(next), "%a %e %b", (time_t)s.start);
            snprintf(detail, sizeof(detail), "next on %s, %s", next, in);
        } else {
            snprintf(detail, sizeof(detail), "%s", v.clockMinute >= 0 ? in : "the clock isn't set");
        }
        drawText(MARGIN, y + 58, &Text17, detail, fg);
    }
    if (scrollTop + visible < v.scheduleCount) {
        int bottom = LIST_TOP + visible * rowH;
        canvas->fillTriangle(cx - 8, bottom + 3, cx + 8, bottom + 3, cx, bottom + 11, INK);
    }
}

static void drawConfirm(const ViewState &v, void (*icon)(), const char *title, const char *detail, const char *action);
static const int CONFIRM_Y = 268; // top of the confirm dialogs

static void drawUnscheduleDialog(const ViewState &v) {
    char detail[48] = "";
    Schedule s;
    for (int i = 0; scheduleGet(i, s); i++) {
        if (s.start != unscheduleStart) continue;
        char when[24], length[12];
        formatLocal(when, sizeof(when), s.daily ? "every day %H:%M" : "%e %b %H:%M", (time_t)s.start);
        formatTime(length, sizeof(length), s.seconds);
        snprintf(detail, sizeof(detail), "%s  -  %s", when, length);
    }
    drawConfirm(v, [] { iconClock(SW / 2, CONFIRM_Y + 52, 30, INK); }, "Remove recording?", detail, "Remove");
}

// "2.4 kHz", "180 Hz"
static void formatHz(char *buf, size_t len, float hz) {
    if (hz < 1000) snprintf(buf, len, "%d Hz", (int)roundf(hz));
    else if (hz < 10000) snprintf(buf, len, "%.1f kHz", hz / 1000);
    else snprintf(buf, len, "%d kHz", (int)roundf(hz / 1000));
}

// "1:03.4"
static void formatSeconds(char *buf, size_t len, float seconds) {
    snprintf(buf, len, "%d:%04.1f", (int)seconds / 60, fmodf(seconds, 60));
}

// Frequency scale of both views: 0 to 20 kHz in 5 kHz steps
static const int FREQ_STEPS = 4;
static const char *const FREQ_LABELS[FREQ_STEPS + 1] = {"0", "5k", "10k", "15k", "20k"};

// Spectrogram: time across, frequency up (linear), loudness as dithered ink.
// Marked with the frequency scale, the time, a ring on the loudest point, and
// the playhead while it plays.
static void drawSpectrogram(const ViewState &v, const Spectrogram *sp) {
    // Loudness as a 4x4 ordered dither: 17 shades from paper to ink
    static const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    for (int y = 0; y < sp->height && y < PLOT_H; y++) {
        const uint8_t *row = sp->cells + y * sp->width;
        for (int x = 0; x < sp->width && x < PLOT_W; x++) {
            if (row[x] * 17 / 256 > BAYER[y & 3][x & 3]) canvas->drawPixel(PLOT_X + x, PLOT_Y + y, INK);
        }
    }
    canvas->drawRect(PLOT_X - 1, PLOT_Y - 1, PLOT_W + 2, PLOT_H + 2, INK);

    for (int i = 0; i <= FREQ_STEPS; i++) {
        int y = PLOT_Y + PLOT_H - 1 - i * (PLOT_H - 1) / FREQ_STEPS;
        canvas->drawFastHLine(PLOT_X - 7, y, 6, INK);
        if (i > 0 && i < FREQ_STEPS) {
            for (int x = PLOT_X; x < PLOT_X + PLOT_W; x += 6) canvas->drawPixel(x, y, INK);
        }
        drawTextRight(PLOT_X - 11, constrain(y + 6, PLOT_Y + 12, PLOT_Y + PLOT_H), &Text17, FREQ_LABELS[i], INK);
    }
    drawTextRight(PLOT_X - 11, PLOT_Y - 8, &Text17, "Hz", INK);

    // Time: start, middle, end
    char buf[48];
    for (int i = 0; i <= 2; i++) {
        int x = PLOT_X + i * (PLOT_W - 1) / 2;
        canvas->drawFastVLine(x, PLOT_Y + PLOT_H + 1, 6, INK);
        formatSeconds(buf, sizeof(buf), sp->seconds * i / 2);
        if (i == 0) drawText(x, PLOT_Y + PLOT_H + 26, &Text17, buf, INK);
        else if (i == 1) drawTextCentered(x, PLOT_Y + PLOT_H + 26, &Text17, buf, INK);
        else drawTextRight(x, PLOT_Y + PLOT_H + 26, &Text17, buf, INK);
    }

    // The loudest point: a ring, paper outside ink so it shows on both
    int px = PLOT_X + sp->peakColumn, py = PLOT_Y + sp->peakRow;
    drawArc(px, py, 13, 0, 360, 3, PAPER);
    drawArc(px, py, 10, 0, 360, 3, INK);
    drawArc(px, py, 7, 0, 360, 2, PAPER);

    // Playhead: an ink line with paper edges, and a marker above the plot
    if (v.audio == AUDIO_PLAYING && sp->seconds > 0) {
        int x = PLOT_X + constrain((int)(v.playTicks / 8.0f / sp->seconds * PLOT_W), 0, PLOT_W - 1);
        canvas->fillRect(x - 2, PLOT_Y, 5, PLOT_H, PAPER);
        canvas->fillRect(x - 1, PLOT_Y, 3, PLOT_H, INK);
        canvas->fillTriangle(x - 7, PLOT_Y - 10, x + 7, PLOT_Y - 10, x, PLOT_Y - 2, INK);
    }

    char hz[16], at[16], low[16], high[16];
    formatHz(hz, sizeof(hz), sp->peakHz);
    formatSeconds(at, sizeof(at), sp->peakSeconds);
    int y = PLOT_Y + PLOT_H + 64;
    drawArc(MARGIN + 9, y - 8, 9, 0, 360, 3, INK);
    snprintf(buf, sizeof(buf), "Loudest: %s, at %s", hz, at);
    drawText(MARGIN + 28, y, &Text22, buf, INK);
    formatHz(low, sizeof(low), sp->bandLowHz);
    formatHz(high, sizeof(high), sp->bandHighHz);
    snprintf(buf, sizeof(buf), "80%% of the sound: %s to %s", low, high);
    drawText(MARGIN, y + 30, &Text22, buf, INK);
}

// FFT graph: the moment's average spectrum, loudness (dB below its loudest
// frequency) up against frequency (linear) across, filled lightly below
static void drawFftGraph(const ViewState &v, const Spectrogram *sp) {
    const float rangeDb = 80;
    int lastBin = min(sp->bins - 1, (int)(SPECTRUM_HIGH_HZ / sp->binHz));
    float loudest = -1000;
    for (int b = 1; b <= lastBin; b++) loudest = max(loudest, sp->averageDb[b]);

    int prevY = -1;
    for (int x = 0; x < PLOT_W; x++) {
        // The loudest bin under this pixel
        int b0 = max(1, (int)(x * SPECTRUM_HIGH_HZ / PLOT_W / sp->binHz));
        int b1 = max(b0, min(lastBin, (int)((x + 1) * SPECTRUM_HIGH_HZ / PLOT_W / sp->binHz)));
        float value = -1000;
        for (int b = b0; b <= b1; b++) value = max(value, sp->averageDb[b]);
        int y = PLOT_Y + constrain((int)((loudest - value) / rangeDb * PLOT_H), 0, PLOT_H - 1);
        for (int yy = y + 2; yy < PLOT_Y + PLOT_H; yy++) {
            if (((x + yy) & 3) == 0) canvas->drawPixel(PLOT_X + x, yy, INK); // light fill
        }
        if (prevY >= 0) canvas->drawLine(PLOT_X + x - 1, prevY, PLOT_X + x, y, INK);
        canvas->drawLine(PLOT_X + x, y + 1, PLOT_X + x, prevY >= 0 ? max(prevY, y) + 1 : y + 1, INK); // 2 px thick
        prevY = y;
    }
    canvas->drawRect(PLOT_X - 1, PLOT_Y - 1, PLOT_W + 2, PLOT_H + 2, INK);

    // dB scale on the left, every 20 dB
    char buf[48];
    for (int db = 0; db <= (int)rangeDb; db += 20) {
        int y = PLOT_Y + (int)(db / rangeDb * (PLOT_H - 1));
        canvas->drawFastHLine(PLOT_X - 7, y, 6, INK);
        if (db > 0 && db < rangeDb) {
            for (int x = PLOT_X; x < PLOT_X + PLOT_W; x += 6) canvas->drawPixel(x, y, INK);
        }
        snprintf(buf, sizeof(buf), db == 0 ? "0" : "-%d", db);
        drawTextRight(PLOT_X - 11, constrain(y + 6, PLOT_Y + 12, PLOT_Y + PLOT_H), &Text17, buf, INK);
    }
    drawTextRight(PLOT_X - 11, PLOT_Y - 8, &Text17, "dB", INK);

    // Frequency across the bottom
    for (int i = 0; i <= FREQ_STEPS; i++) {
        int x = PLOT_X + i * (PLOT_W - 1) / FREQ_STEPS;
        canvas->drawFastVLine(x, PLOT_Y + PLOT_H + 1, 6, INK);
        if (i > 0 && i < FREQ_STEPS) {
            for (int y = PLOT_Y; y < PLOT_Y + PLOT_H; y += 6) canvas->drawPixel(x, y, INK);
        }
        const char *label = FREQ_LABELS[i];
        if (i == 0) drawText(x, PLOT_Y + PLOT_H + 26, &Text17, label, INK);
        else if (i == FREQ_STEPS) drawTextRight(x, PLOT_Y + PLOT_H + 26, &Text17, "20k Hz", INK);
        else drawTextCentered(x, PLOT_Y + PLOT_H + 26, &Text17, label, INK);
    }

    // The loudest frequency on average: a ring on the curve
    int px = PLOT_X + constrain((int)(sp->averagePeakHz / SPECTRUM_HIGH_HZ * PLOT_W), 0, PLOT_W - 1);
    drawArc(px, PLOT_Y + 1, 13, 0, 360, 3, PAPER);
    drawArc(px, PLOT_Y + 1, 10, 0, 360, 3, INK);

    char hz[16], low[16], high[16];
    formatHz(hz, sizeof(hz), sp->averagePeakHz);
    int y = PLOT_Y + PLOT_H + 64;
    drawArc(MARGIN + 9, y - 8, 9, 0, 360, 3, INK);
    snprintf(buf, sizeof(buf), "Loudest on average: %s", hz);
    drawText(MARGIN + 28, y, &Text22, buf, INK);
    formatHz(low, sizeof(low), sp->bandLowHz);
    formatHz(high, sizeof(high), sp->bandHighHz);
    snprintf(buf, sizeof(buf), "80%% of the sound: %s to %s", low, high);
    drawText(MARGIN, y + 30, &Text22, buf, INK);
}

static void drawSpectrum(const ViewState &v) {
    Moment moment;
    if (!momentGet(momentIndexByNumber(v.spectrumMoment), moment)) return;
    char title[24];
    snprintf(title, sizeof(title), "Moment %d", moment.number);
    drawSubHeader(v, title, v.spectrumView == VIEW_FFT ? "Spectrum (FFT)" : "Spectrogram");
    const Spectrogram *sp = spectrumResult(v.spectrumMoment);
    if (!sp) {
        iconBars(SW / 2, 300, 30, INK);
        char buf[24];
        snprintf(buf, sizeof(buf), "Analysing... %d%%", v.spectrumProgress);
        drawTextCentered(SW / 2, 380, &Bold22, buf, INK);
        const int bw = 300, bh = 18, bx = SW / 2 - bw / 2, by = 410;
        canvas->drawRoundRect(bx, by, bw, bh, 6, INK);
        canvas->drawRoundRect(bx + 1, by + 1, bw - 2, bh - 2, 5, INK);
        if (v.spectrumProgress > 0) canvas->fillRoundRect(bx + 3, by + 3, (bw - 6) * v.spectrumProgress / 100, bh - 6, 4, INK);
        return;
    }
    if (v.spectrumView == VIEW_FFT) drawFftGraph(v, sp);
    else drawSpectrogram(v, sp);
}

static void drawCardReader(const ViewState &v) {
    drawSubHeader(v, "USB card reader", "The SD card is open on the computer", false);
    int cx = SW / 2;
    iconSdCard(cx + 24, 260, 3);
    if (v.cardEjected) {
        drawTextCentered(cx, 380, &Bold32, "Ejected", INK);
        drawTextCentered(cx, 418, &Text22, "Safe to leave", INK);
    } else {
        drawTextCentered(cx, 380, &Bold32, "Connected", INK);
        drawTextCentered(cx, 418, &Text22, "Eject the drive on the computer", INK);
        drawTextCentered(cx, 446, &Text22, "before leaving", INK);
    }
    drawTextCentered(cx, 520, &Text17, "Leaving restarts zvukopis, which reads", INK);
    drawTextCentered(cx, 542, &Text17, "the card's moments again", INK);
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
    drawTextCentered(cx, 368, &Bold44, saving ? "Saving" : "Recording", INK);

    char limit[16], started[12], buf[48];
    formatTimeLeft(limit, sizeof(limit), max(1, (int)audioRecordingLimitSeconds()));
    if (v.clockMinute >= 0) {
        formatLocal(started, sizeof(started), "%H:%M", (time_t)v.clockMinute * 60);
        snprintf(buf, sizeof(buf), "started %s  -  up to %s", started, limit);
    } else {
        snprintf(buf, sizeof(buf), "up to %s", limit);
    }
    drawTextCentered(cx, 412, &Text22, buf, INK);
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
    const int x = (SW - w) / 2, y = CONFIRM_Y, cx = SW / 2;
    dialogBox(y, h);
    icon();
    drawTextCentered(cx, y + 134, &Bold32, title, INK);
    drawTextCentered(cx, y + 166, &Text22, detail, INK);

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
        drawTextCentered(bx + bw / 2, by + 32, &Bold22, labels[i], chosen ? PAPER : INK);
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
    drawConfirm(v, [] { iconTrash(SW / 2, CONFIRM_Y + 58); }, title, detail, "Delete");
}

static void drawFormatDialog(const ViewState &v) {
    char detail[48];
    int onCard = 0;
    Moment moment;
    for (int i = 0; momentGet(i, moment); i++) {
        if (!moment.samples) onCard++;
    }
    if (storageCardState() == SD_UNREADABLE) snprintf(detail, sizeof(detail), "Erases the card so zvukopis can use it");
    else snprintf(detail, sizeof(detail), "Erases %d moment%s and all else", onCard, onCard == 1 ? "" : "s");
    drawConfirm(v, [] { iconSdCard(SW / 2 + 24, CONFIRM_Y + 62, 3); }, "Format SD card?", detail, "Format");
}

// Auto, or a fixed gain
static void drawGainPicker(const ViewState &v) {
    shadeBehindDialog();
    const int itemH = 40, y = 126, h = 70 + GAIN_CHOICE_COUNT * itemH + 12;
    const int w = SW - 2 * MARGIN - 16, x = (SW - w) / 2;
    dialogBox(y, h);
    drawText(x + 24, y + 44, &Bold32, "Mic gain", INK);
    for (int i = 0; i < GAIN_CHOICE_COUNT; i++) {
        int iy = y + 62 + i * itemH;
        bool chosen = i == v.overlayChoice;
        uint16_t fg = chosen ? PAPER : INK;
        if (chosen) canvas->fillRoundRect(x + 12, iy, w - 24, itemH - 4, 10, INK);
        char label[48];
        if (GAIN_CHOICES[i] == GAIN_AUTO) snprintf(label, sizeof(label), "Auto  (adjusts while recording)");
        else snprintf(label, sizeof(label), "%d dB%s", GAIN_CHOICES[i], GAIN_CHOICES[i] == 54 ? "  (quiet places)" : GAIN_CHOICES[i] == 0 ? "  (very loud)" : "");
        drawText(x + 56, iy + 26, &Text22, label, fg);
        if (GAIN_CHOICES[i] == v.gainSetting) canvas->fillCircle(x + 34, iy + 18, 5, fg);
    }
}

// Tag the moment, or delete it
static void drawMenu(const ViewState &v) {
    int index = momentIndexByNumber(v.menuMoment);
    Moment moment;
    if (!momentGet(index, moment)) return;
    shadeBehindDialog();
    const int itemH = 50, y = 140, h = 76 + MENU_ITEMS * itemH + 14;
    const int w = SW - 2 * MARGIN - 16, x = (SW - w) / 2;
    dialogBox(y, h);
    char title[32];
    snprintf(title, sizeof(title), "Moment %d", moment.number);
    drawText(x + 24, y + 46, &Bold32, title, INK);

    for (int i = 0; i < MENU_ITEMS; i++) {
        int iy = y + 70 + i * itemH;
        bool chosen = i == v.overlayChoice;
        uint16_t fg = chosen ? PAPER : INK;
        if (chosen) canvas->fillRoundRect(x + 12, iy, w - 24, itemH - 4, 10, INK);
        int cy = iy + (itemH - 4) / 2;
        const char *label;
        if (i == MENU_ANALYSIS && !chosen) dottedLine(x + 24, x + w - 24, iy - 2); // tags above, actions below
        if (i < MENU_ANALYSIS) {
            int tag = TAG_HUMAN + i;
            iconTag(tag, x + 44, cy, 13, fg);
            label = configCategory(tag).name;
            if (moment.tag == tag) {
                // Check mark: choosing it again removes the tag
                canvas->drawLine(x + w - 56, cy, x + w - 48, cy + 8, fg);
                canvas->drawLine(x + w - 56, cy + 1, x + w - 48, cy + 9, fg);
                canvas->drawLine(x + w - 48, cy + 8, x + w - 34, cy - 8, fg);
                canvas->drawLine(x + w - 48, cy + 9, x + w - 34, cy - 7, fg);
            }
        } else if (i == MENU_ANALYSIS) {
            iconBars(x + 44, cy, 12, fg);
            label = "Frequency analysis";
        } else if (i == MENU_DELETE) {
            label = "Delete...";
        } else {
            label = "Cancel";
        }
        drawText(x + 76, cy + 8, i < MENU_ANALYSIS ? &Bold22 : &Text22, label, fg);
    }
}

// Opening quotation mark, two "6" shapes; x, y is its top left. Returns its height.
static int drawQuoteMark(int x, int y) {
    for (int i = 0; i < 2; i++) {
        int cx = x + 10 + i * 26, cy = y + 24;
        canvas->fillCircle(cx, cy, 9, INK);
        canvas->fillTriangle(cx - 9, cy - 1, cx - 2, cy - 1, cx + 8, cy - 24, INK); // the tail
        canvas->fillTriangle(cx - 9, cy - 1, cx + 8, cy - 24, cx + 11, cy - 21, INK);
    }
    return 34;
}

// The quote is the page: left-aligned like a book, as large as it fits,
// centred between the header (asleep since when, what it holds) and the
// footer (how to wake)
static void drawSleepScreen(const ViewState &v) {
    drawText(MARGIN, TITLE_BASELINE, &Bold22, "zvukopis", INK);
    drawStatus(SW - MARGIN, TITLE_BASELINE, v);
    char buf[48];
    if (v.clockMinute >= 0) {
        char since[12];
        formatLocal(since, sizeof(since), "%H:%M", (time_t)v.clockMinute * 60);
        snprintf(buf, sizeof(buf), "Asleep since %s", since);
    } else {
        snprintf(buf, sizeof(buf), "Asleep");
    }
    drawText(MARGIN, SECOND_BASELINE, &Text22, buf, INK);
    if (v.nextScheduleMinute >= 0) {
        char when[16];
        time_t next = (time_t)v.nextScheduleMinute * 60;
        bool today = v.clockMinute >= 0 && dayKey(next) == dayKey((time_t)v.clockMinute * 60);
        formatLocal(when, sizeof(when), today ? "%H:%M" : "%e %b %H:%M", next);
        snprintf(buf, sizeof(buf), "records at %s", when);
    } else {
        snprintf(buf, sizeof(buf), "%d moment%s", v.momentCount, v.momentCount == 1 ? "" : "s");
    }
    drawTextRight(SW - MARGIN, SECOND_BASELINE, &Text22, buf, INK);
    canvas->fillRect(MARGIN, HEADER_LINE_Y, SW - 2 * MARGIN, 2, INK);

    // How to wake: the PWR button is on the right edge, about 5 mm above the
    // rocker (which sits beside the first list row), so the pointer goes there
    const int py = LIST_TOP + 4;
    canvas->fillRect(SW - 30, py - 2, 16, 5, INK);
    canvas->fillTriangle(SW - 14, py - 8, SW - 14, py + 8, SW - 4, py, INK);
    drawTextRight(SW - 42, py + 8, &Text22, "Hold 2 s to wake", INK);

    // "Pauline Oliveros, Deep Listening" -> author, then the work below
    const Quote &quote = QUOTES[v.quote % QUOTE_COUNT];
    char author[64];
    strlcpy(author, quote.source, sizeof(author));
    char *comma = strchr(author, ',');
    const char *work = nullptr;
    if (comma) {
        *comma = 0;
        work = quote.source + (comma - author) + 1;
        while (*work == ' ') work++;
    }

    const int top = LIST_TOP + 36, bottom = SH - 36, width = SW - 2 * MARGIN;
    struct Size {
        const GFXfont *font;
        int lineHeight;
    };
    static const Size sizes[] = {{&Italic40, 52}, {&Italic33, 44}, {&Italic27, 36}, {&Italic22, 30}};
    const int markH = 34 + 14, byline = 22 + 34 + (work ? 26 : 0);
    const Size *size = &sizes[0];
    int textH = 0;
    for (const Size &s : sizes) {
        size = &s;
        textH = drawWrapped(MARGIN, 0, width, s.lineHeight, s.font, quote.text, false, false);
        if (markH + textH + byline <= bottom - top) break;
    }
    int y = top + max(0, (bottom - top - markH - textH - byline) / 2);
    y += drawQuoteMark(MARGIN, y) + 14;
    y = drawWrapped(MARGIN, y + size->lineHeight - 12, width, size->lineHeight, size->font, quote.text, true, false);
    y += 22 - size->lineHeight + 12;
    canvas->fillRect(MARGIN, y, 40, 2, INK);
    drawText(MARGIN, y + 30, &Bold22, author, INK);
    if (work) drawText(MARGIN, y + 56, &Text17, work, INK);
}

// Context hints: what the rocker does on the first line, BOOT and PWR on the second
static void drawFooter(const ViewState &v) {
    struct Hint {
        HintIcon icon;
        const char *label;
    } rocker[3], keys[2];
    int nr = 0, nk = 0;
    bool menuKeys = true; // BOOT for the menu, PWR to sleep
    bool playing = v.audio == AUDIO_PLAYING;
    if (v.audio == AUDIO_SAVING || v.audio == AUDIO_FORMATTING) {
        menuKeys = false; // nothing to press meanwhile
    } else if (v.audio == AUDIO_RECORDING) {
        rocker[nr++] = {HINT_CLICK, "Stop recording"};
        menuKeys = false;
    } else if (v.overlay != OVERLAY_NONE) {
        // The press label says what will happen to the highlighted item
        const char *press = "Choose";
        if (v.overlay == OVERLAY_MENU) {
            Moment moment;
            if (v.overlayChoice < MENU_ANALYSIS) {
                bool tagged = momentGet(momentIndexByNumber(v.menuMoment), moment) && moment.tag == TAG_HUMAN + v.overlayChoice;
                press = tagged ? "Remove tag" : "Tag";
            } else if (v.overlayChoice == MENU_ANALYSIS) {
                press = "Open";
            } else {
                press = v.overlayChoice == MENU_DELETE ? "Delete..." : "Cancel";
            }
        } else if (v.overlay == OVERLAY_GAIN) {
            press = "Use";
        } else if (v.overlayChoice == 0) {
            press = "Cancel";
        } else {
            press = v.overlay == OVERLAY_CONFIRM_DELETE ? "Delete" : v.overlay == OVERLAY_CONFIRM_FORMAT ? "Format" : "Remove";
        }
        rocker[nr++] = {HINT_UPDOWN, "Select"};
        rocker[nr++] = {HINT_CLICK, press};
        rocker[nr++] = {HINT_HOLD_CLICK, "Hold: cancel"};
        menuKeys = false; // BOOT still closes it; the hold hint already says how to leave
    } else if (v.screen == SCREEN_TRANSFER) {
        rocker[nr++] = {HINT_CLICK, "Stop the web interface"};
        menuKeys = false;
    } else if (v.screen == SCREEN_CARD_READER) {
        rocker[nr++] = {HINT_CLICK, "Leave (restarts)"};
        menuKeys = false;
    } else if (v.screen == SCREEN_SETTINGS) {
        static const char *const presses[SET_ROWS] = {"Change", "Open", "Start", "Start", "Sync now", "Start", "Format..."};
        rocker[nr++] = {HINT_UPDOWN, "Select"};
        rocker[nr++] = {HINT_CLICK, v.selected == ROW_BACK ? "Back" : presses[v.selected]};
        rocker[nr++] = {HINT_HOLD_CLICK, "Hold: back"};
    } else if (v.screen == SCREEN_SPECTRUM) {
        if (playing) rocker[nr++] = {HINT_UPDOWN, "Up or down: speed"};
        else rocker[nr++] = {HINT_UPDOWN, v.spectrumView == VIEW_FFT ? "Spectrogram" : "FFT"};
        rocker[nr++] = {HINT_CLICK, playing ? "Stop" : "Play"};
        rocker[nr++] = {HINT_HOLD_CLICK, "Hold: back"};
    } else if (v.screen == SCREEN_SCHEDULE) {
        rocker[nr++] = {HINT_UPDOWN, "Select"};
        rocker[nr++] = {HINT_CLICK, "Back"};
        if (v.selected >= 0) rocker[nr++] = {HINT_HOLD_CLICK, "Hold: remove"};
    } else if (v.screen == SCREEN_CALENDAR) {
        rocker[nr++] = {HINT_UPDOWN, "Day"};
        rocker[nr++] = {HINT_CLICK, v.selected == ROW_BACK ? "Back" : "Open day"};
        rocker[nr++] = {HINT_HOLD_CLICK, "Hold: back"};
    } else if (playing && (v.screen == SCREEN_LIST || v.screen == SCREEN_DAY)) {
        rocker[nr++] = {HINT_UPDOWN, "Up or down: speed"};
        rocker[nr++] = {HINT_CLICK, "Stop"}; // the hold hint is back once stopped (its row would overflow)
    } else if (v.selected < 0) {
        rocker[nr++] = {HINT_UPDOWN, "Select"};
        rocker[nr++] = {HINT_CLICK, v.screen == SCREEN_DAY ? "Back" : "Open calendar"};
    } else if (v.screen == SCREEN_LIST && v.selected == ROW_NEXT_SCHEDULED && v.scheduleCount > 0) {
        rocker[nr++] = {HINT_UPDOWN, "Select"};
        rocker[nr++] = {HINT_CLICK, "All scheduled"};
    } else if (v.screen == SCREEN_LIST && v.selected == 0) {
        rocker[nr++] = {HINT_UPDOWN, "Select"};
        rocker[nr++] = {HINT_CLICK, "Record"};
    } else {
        rocker[nr++] = {HINT_UPDOWN, "Select"};
        rocker[nr++] = {HINT_CLICK, "Play"};
        rocker[nr++] = {HINT_HOLD_CLICK, "Hold: actions"};
    }
    if (menuKeys) {
        keys[nk++] = {HINT_BOOT, v.screen == SCREEN_SETTINGS || v.overlay != OVERLAY_NONE ? "Close" : "Settings"};
        keys[nk++] = {HINT_PWR, "Hold 2 s to sleep"};
    }

    // Rocker hints read "gesture: what happens", like the legend on the empty
    // screen; they flow over up to two lines, the keys take the third
    char labels[3][40];
    for (int i = 0; i < nr; i++) {
        const char *prefix = rocker[i].icon == HINT_UPDOWN ? "Up or down: " : rocker[i].icon == HINT_CLICK ? "Press in: " : "Hold in: ";
        const char *what = rocker[i].label;
        if (strncmp(what, "Hold: ", 6) == 0) what += 6;
        else if (strncmp(what, "Up or down: ", 12) == 0) what += 12;
        bool acronym = isupper((uint8_t)what[0]) && isupper((uint8_t)what[1]); // "FFT" stays as it is
        snprintf(labels[i], sizeof(labels[i]), "%s%c%s", prefix, acronym ? what[0] : tolower((uint8_t)what[0]), what + 1);
        rocker[i].label = labels[i];
    }
    canvas->drawFastHLine(MARGIN, FOOTER_LINE_Y, SW - 2 * MARGIN, INK);
    const int gap = 22, lineH = 34, maxLines = 3;
    int x = MARGIN, line = 0;
    auto place = [&](HintIcon icon, const char *label) {
        int iconW = icon == HINT_UPDOWN ? 20 : icon == HINT_CLICK ? 26 : icon == HINT_HOLD_CLICK ? 40
                    : textWidth(&Text17, icon == HINT_PWR ? "PWR" : "BOOT") + 12 + 8;
        int w = iconW + textWidth(&Text22, label);
        if (x > MARGIN && x + w > SW - MARGIN) {
            x = MARGIN;
            line++;
        }
        if (line >= maxLines) return;
        drawHint(x, FOOTER_LINE_Y + 28 + line * lineH, icon, label);
        x += w + gap;
    };
    for (int i = 0; i < nr; i++) place(rocker[i].icon, rocker[i].label);
    if (nk > 0 && x > MARGIN) {
        x = MARGIN;
        line++;
    }
    for (int i = 0; i < nk; i++) place(keys[i].icon, keys[i].label);
}

static void render(const ViewState &v) {
    canvas->fillScreen(PAPER);
    if (v.sleeping) {
        drawSleepScreen(v); // its own pointer says how to wake, so no footer
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
        case SCREEN_SCHEDULE: drawScheduleScreen(v); break;
        case SCREEN_SPECTRUM: drawSpectrum(v); break;
        }
        switch (v.overlay) {
        case OVERLAY_MENU: drawMenu(v); break;
        case OVERLAY_CONFIRM_DELETE: drawDeleteDialog(v); break;
        case OVERLAY_CONFIRM_FORMAT: drawFormatDialog(v); break;
        case OVERLAY_GAIN: drawGainPicker(v); break;
        case OVERLAY_CONFIRM_UNSCHEDULE: drawUnscheduleDialog(v); break;
        }
    }
    drawFooter(v);
}

// --- Panel output ---

static void pushPartial(const ViewState &v) {
    uint32_t start = millis();
    Serial.printf("[display] %lu ms: refresh (screen %d, selected %d, moments %d, audio %d, overlay %d)\n",
                  start, v.screen, v.selected, v.momentCount, v.audio, v.overlay);
    // The whole buffer every time: a sub-window made the panel show the band in
    // the wrong place, and the transfer is only ~40 ms of the ~0.5 s update
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
    case BTN_MENU: return "menu";
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
    selected = which == SCREEN_LIST ? 0 : row; // the main screen always opens on New recording, ready to record
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
            if (overlayChoice == MENU_ANALYSIS) {
                spectrumFrom = (Screen)(int)screen;
                spectrumFromSelected = selected;
                spectrumMoment = menuMoment;
                spectrumView = VIEW_SPECTROGRAM;
                spectrumRequest(menuMoment, PLOT_W, PLOT_H);
                overlay = OVERLAY_NONE;
                showScreen(SCREEN_SPECTRUM, 0);
            } else if (overlayChoice < MENU_ANALYSIS) {
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
        } else if (overlay == OVERLAY_CONFIRM_UNSCHEDULE) {
            Schedule s;
            for (int i = 0; overlayChoice == 1 && scheduleGet(i, s); i++) {
                if (s.start == unscheduleStart) {
                    scheduleRemove(i);
                    break;
                }
            }
            overlay = OVERLAY_NONE;
        }
        break;
    }
    case BTN_LONG_CLICK:
    case BTN_MENU:
        overlay = OVERLAY_NONE; // same as the Cancel row
        break;
    default:
        break;
    }
}

static void openSchedule(Screen from) {
    scheduleFrom = from;
    showScreen(SCREEN_SCHEDULE, scheduleCount() > 0 ? 0 : ROW_BACK);
}

static void openSettings() {
    menuFrom = (Screen)(int)screen;
    menuFromSelected = selected;
    showScreen(SCREEN_SETTINGS, 0);
}

static void closeSettings() { showScreen(menuFrom, menuFromSelected); }

static void handleList(ButtonEvent event) {
    bool scheduled = scheduleCount() > 0;
    int first = firstMomentRow(scheduled);
    switch (event) {
    case BTN_UP:
        if (selected > (momentCount() > 0 ? ROW_CALENDAR : 0)) selected = selected - 1;
        break;
    case BTN_DOWN:
        if (selected < first + momentCount() - 1) selected = selected + 1;
        break;
    case BTN_CLICK:
        if (selected == ROW_CALENDAR) openCalendar();
        else if (selected == 0) audioStartRecording();
        else if (selected < first) openSchedule(SCREEN_LIST);
        else togglePlay(selected - first);
        break;
    case BTN_LONG_CLICK:
        if (selected >= first) openMenu(selected - first);
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

static void handleSchedule(ButtonEvent event) {
    Schedule s;
    switch (event) {
    case BTN_UP:
        if (selected > ROW_BACK) selected = selected - 1;
        break;
    case BTN_DOWN:
        if (selected < scheduleCount() - 1) selected = selected + 1;
        break;
    case BTN_CLICK:
        showScreen(scheduleFrom, scheduleFrom == SCREEN_SETTINGS ? SET_SCHEDULE : ROW_NEXT_SCHEDULED);
        break;
    case BTN_LONG_CLICK:
        if (selected >= 0 && scheduleGet(selected, s)) {
            unscheduleStart = s.start;
            openOverlay(OVERLAY_CONFIRM_UNSCHEDULE, 0);
        } else {
            showScreen(scheduleFrom, scheduleFrom == SCREEN_SETTINGS ? SET_SCHEDULE : ROW_NEXT_SCHEDULED);
        }
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
        closeSettings();
        break;
    case BTN_CLICK:
        switch (selected) {
        case ROW_BACK:
            closeSettings();
            break;
        case SET_SYNC:
            clockSyncNow();
            break;
        case SET_SCHEDULE:
            openSchedule(SCREEN_SETTINGS);
            break;
        case SET_GAIN: {
            int current = 0;
            for (int i = 0; i < GAIN_CHOICE_COUNT; i++) {
                if (GAIN_CHOICES[i] == audioMicGainSetting()) current = i;
            }
            openOverlay(OVERLAY_GAIN, current);
            break;
        }
        case SET_WEB:
        case SET_HOTSPOT:
            if (clockSyncing()) break; // it has Wi-Fi for now
            transferRow = selected;
            transferStart(selected == SET_HOTSPOT);
            showScreen(SCREEN_TRANSFER, 0);
            break;
        case SET_CARD_READER:
            if (storageCardState() == SD_ABSENT || !usbPowered()) break;
            audioSuspend(); // the audio task lets go of the card
            if (cardReaderStart()) showScreen(SCREEN_CARD_READER, 0);
            break;
        case SET_FORMAT:
            if (storageCardState() != SD_ABSENT && audioState() != AUDIO_FORMATTING) openOverlay(OVERLAY_CONFIRM_FORMAT, 0);
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
            showScreen(SCREEN_SETTINGS, transferRow);
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
    // BOOT opens the menu from anywhere, and closes it again
    if (event == BTN_MENU) {
        if (screen == SCREEN_SETTINGS) closeSettings();
        else if (screen != SCREEN_TRANSFER) openSettings();
        return;
    }
    // Up and down are the tape transport while a moment plays
    if (audio == AUDIO_PLAYING && (event == BTN_UP || event == BTN_DOWN) &&
        (screen == SCREEN_LIST || screen == SCREEN_DAY || screen == SCREEN_SPECTRUM)) {
        audioSpeedHold(event == BTN_UP ? 1 : -1);
        return;
    }

    switch (screen) {
    case SCREEN_LIST: handleList(event); break;
    case SCREEN_DAY: handleDay(event); break;
    case SCREEN_CALENDAR: handleCalendar(event); break;
    case SCREEN_SETTINGS: handleSettings(event); break;
    case SCREEN_SCHEDULE: handleSchedule(event); break;
    case SCREEN_SPECTRUM:
        if (event == BTN_UP || event == BTN_DOWN) {
            spectrumView = spectrumView == VIEW_FFT ? VIEW_SPECTROGRAM : VIEW_FFT;
        } else if (event == BTN_CLICK) {
            int index = momentIndexByNumber(spectrumMoment);
            if (index >= 0) togglePlay(index);
        } else if (event == BTN_LONG_CLICK) {
            if (audio == AUDIO_PLAYING) audioStopPlayback();
            showScreen(spectrumFrom, spectrumFromSelected);
        }
        break;
    case SCREEN_TRANSFER:
        if (event == BTN_CLICK) {
            transferStop();
            showScreen(SCREEN_SETTINGS, transferRow);
        }
        break;
    }
}

void uiTick() {
    int count = momentCount();
    // The next-scheduled row comes and goes; keep the same moment selected
    static bool wasScheduled = false;
    bool scheduled = scheduleCount() > 0;
    int first = firstMomentRow(scheduled);
    if (screen == SCREEN_LIST && scheduled != wasScheduled && selected >= 1) selected = selected + (scheduled ? 1 : -1);
    wasScheduled = scheduled;
    if (count > lastMomentCount && screen == SCREEN_LIST) selected = 0; // after a recording (or at boot): ready to record again
    lastMomentCount = count;
    if (screen == SCREEN_LIST && selected > first + count - 1) selected = first + count - 1;
    if (screen == SCREEN_SCHEDULE && selected >= scheduleCount()) selected = scheduleCount() - 1; // ROW_BACK when empty
    static uint32_t spectrumCheckedVersion = UINT32_MAX;
    if (screen == SCREEN_SPECTRUM && momentListVersion() != spectrumCheckedVersion) {
        spectrumCheckedVersion = momentListVersion();
        if (momentIndexByNumber(spectrumMoment) < 0) showScreen(spectrumFrom, 0); // deleted meanwhile
    }
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
        showScreen(SCREEN_SETTINGS, transferRow); // stopped after 10 minutes unused
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
    if (screen == SCREEN_SPECTRUM && !spectrumResult(spectrumMoment)) return false; // still working it out
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

bool uiCanRecord() { return screen != SCREEN_CARD_READER; }

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
