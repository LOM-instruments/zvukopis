#include "ui.h"

#include <Adafruit_GFX.h>
#include <Arduino.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <mbedtls/base64.h>

#include "EPD_3in97.h"
#include "audio.h"
#include "board_power.h"

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

static const int FULL_REFRESH_EVERY = 30; // partial refreshes, before a full one clears ghosting
static const uint32_t CONFIRM_TIMEOUT_MS = 10000;
static const uint32_t SLEEP_SCREEN_TIMEOUT_MS = 15000;
static const uint32_t BATTERY_READ_MS = 30000;

// UI state: written by loop(), read by the display task
static volatile int selected = 0; // row 0 = "New recording", row n = memo n-1
static volatile bool confirmDelete = false;
static volatile int confirmChoice = 0; // 0 = Cancel, 1 = Delete
static uint32_t confirmSince = 0;
static volatile bool sleepRequested = false;
static volatile bool sleeping = false;    // what the screen should show
static volatile bool panelAsleep = false; // what the display task has done
static int lastMemoCount = 0;
static volatile bool screenshotRequested = false;

static GFXcanvas1 *canvas;

// Everything the screen depends on; the display task redraws when it changes
struct ViewState {
    int selected;
    int confirmDelete;
    int confirmChoice;
    int sleeping;
    int memoCount;
    uint32_t listVersion;
    int audio;
    int playingIndex;
    int playSeconds;
    int recordSeconds;
    int secondsLeft;
    int battery;
    int usb;
    int sdCard;
};

static ViewState currentView() {
    static int battery = -1, usb = 0;
    static uint32_t batteryReadAt = 0;
    if (batteryReadAt == 0 || millis() - batteryReadAt > BATTERY_READ_MS) {
        battery = batteryPercent();
        usb = usbPowered();
        batteryReadAt = millis();
    }

    ViewState v;
    memset(&v, 0, sizeof(v)); // compared with memcmp
    v.selected = selected;
    v.confirmDelete = confirmDelete;
    v.confirmChoice = confirmChoice;
    v.sleeping = sleeping;
    v.memoCount = memoCount();
    v.listVersion = memoListVersion();
    v.audio = audioState();
    v.playingIndex = audioPlayingIndex();
    v.playSeconds = (int)audioPlaybackSeconds();
    v.recordSeconds = (int)audioRecordingSeconds();
    v.secondsLeft = (int)audioSecondsLeft();
    v.battery = battery;
    v.usb = usb;
    v.sdCard = audioUsingSdCard();
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

static int memoSeconds(const Memo &memo) {
    return max(1, (int)((memo.length + SAMPLE_RATE / 2) / SAMPLE_RATE));
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

static void dottedLine(int x0, int x1, int y) {
    for (int x = x0; x <= x1; x += 4) canvas->drawPixel(x, y, INK);
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
static int iconSdCard(int right, int cy) {
    const int w = 16, h = 20;
    int x = right - w, y = cy - h / 2;
    canvas->fillRect(x, y, w, h, INK);
    canvas->fillTriangle(x + w - 6, y, x + w, y, x + w, y + 6, PAPER); // clipped corner
    for (int i = 0; i < 3; i++) canvas->fillRect(x + 3 + 3 * i, y + 3, 2, 5, PAPER);
    return x;
}

static void iconMoon(int cx, int cy, int r) {
    canvas->fillCircle(cx, cy, r, INK);
    canvas->fillCircle(cx + r / 2, cy - r / 3, r * 5 / 6, PAPER);
}

static void iconStar(int cx, int cy, int r) {
    canvas->fillTriangle(cx - r, cy, cx + r, cy, cx, cy - r / 3, INK);
    canvas->fillTriangle(cx - r, cy, cx + r, cy, cx, cy + r / 3, INK);
    canvas->fillTriangle(cx, cy - r, cx, cy + r, cx - r / 3, cy, INK);
    canvas->fillTriangle(cx, cy - r, cx, cy + r, cx + r / 3, cy, INK);
}

static void iconTrash(int cx, int cy) {
    canvas->fillRoundRect(cx - 28, cy - 34, 56, 6, 2, INK); // lid
    canvas->fillRect(cx - 9, cy - 40, 18, 6, INK);           // handle
    for (int i = 0; i < 3; i++) canvas->drawRect(cx - 22 + i, cy - 22 + i, 44 - 2 * i, 56 - 2 * i, INK);
    for (int dx = -10; dx <= 10; dx += 10) canvas->fillRect(cx + dx - 1, cy - 12, 3, 36, INK);
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

enum HintIcon { HINT_UPDOWN, HINT_CLICK, HINT_HOLD_CLICK, HINT_BOOT, HINT_PWR };

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
    case HINT_BOOT:
        x += drawKeycap(x, cy, "BOOT") + 8;
        break;
    case HINT_PWR:
        x += drawKeycap(x, cy, "PWR") + 8;
        break;
    }
    drawText(x, cy + 7, &FreeSans12pt7b, label, INK);
}

// --- Screen sections ---

static void drawHeader(const ViewState &v) {
    drawText(MARGIN, 50, &FreeSansBold18pt7b, "Voice Memos", INK);

    int x = SW - MARGIN;
    char buf[32];
    if (v.battery >= 0) {
        x = iconBattery(x, 39, v.battery) - 8;
        snprintf(buf, sizeof(buf), "%d%%", v.battery);
        drawTextRight(x, 47, &FreeSans12pt7b, buf, INK);
        x -= textWidth(&FreeSans12pt7b, buf) + 6;
    }
    if (v.usb) {
        iconBolt(x - 6, 38);
        x -= 22;
    }
    if (v.sdCard) iconSdCard(x, 38);

    // "2 memos  •  3:04 left"
    if (v.memoCount == 1) snprintf(buf, sizeof(buf), "1 memo");
    else snprintf(buf, sizeof(buf), "%d memos", v.memoCount);
    drawText(MARGIN, 84, &FreeSans12pt7b, buf, INK);
    x = MARGIN + textWidth(&FreeSans12pt7b, buf) + 14;
    canvas->fillCircle(x, 78, 2, INK);
    if (v.secondsLeft < 1) {
        snprintf(buf, sizeof(buf), v.sdCard ? "card full" : "memory full");
    } else {
        char left[16];
        formatTimeLeft(left, sizeof(left), v.secondsLeft);
        snprintf(buf, sizeof(buf), "%s left", left);
    }
    drawText(x + 14, 84, &FreeSans12pt7b, buf, INK);

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

// Two lines: name and duration, then a waveform. While playing, the part not
// yet played is drawn hollow.
static void drawMemoRow(int index, int y, uint16_t fg, const ViewState &v) {
    Memo memo;
    if (!memoGet(index, memo)) return;
    bool playing = v.audio == AUDIO_PLAYING && v.playingIndex == index;

    int lineY = y + 40;
    if (playing) iconStop(MARGIN + 14, lineY - 8, fg);
    else iconPlay(MARGIN + 14, lineY - 8, fg);

    char buf[32];
    snprintf(buf, sizeof(buf), "Memo %d", memo.number);
    drawText(MARGIN + 44, lineY, &FreeSansBold12pt7b, buf, fg);

    char total[12];
    formatTime(total, sizeof(total), memoSeconds(memo));
    if (playing) {
        char elapsed[12];
        formatTime(elapsed, sizeof(elapsed), v.playSeconds);
        snprintf(buf, sizeof(buf), "%s / %s", elapsed, total);
    } else {
        snprintf(buf, sizeof(buf), "%s", total);
    }
    drawTextRight(SW - MARGIN, lineY, &FreeSans12pt7b, buf, fg);

    int waveY = y + 68;
    int progressBin = WAVEFORM_BINS;
    if (playing) progressBin = (int)((float)v.playSeconds * SAMPLE_RATE / memo.length * WAVEFORM_BINS);
    for (int b = 0; b < WAVEFORM_BINS; b++) {
        int x = WAVE_X + b * WAVE_PITCH;
        int h = 1 + memo.waveform[b] * WAVE_HALF_H / 255;
        if (b < progressBin) canvas->fillRect(x, waveY - h, 3, 2 * h + 1, fg);
        else canvas->drawRect(x, waveY - h, 3, 2 * h + 1, fg);
    }
}

static void drawList(const ViewState &v) {
    static int scrollTop = 0;
    int rows = 1 + v.memoCount;
    if (v.selected < scrollTop) scrollTop = v.selected;
    if (v.selected >= scrollTop + VISIBLE_ROWS) scrollTop = v.selected - VISIBLE_ROWS + 1;
    scrollTop = constrain(scrollTop, 0, max(0, rows - VISIBLE_ROWS));

    for (int i = 0; i < VISIBLE_ROWS && scrollTop + i < rows; i++) {
        int row = scrollTop + i;
        int y = LIST_TOP + i * ROW_H;
        bool isSelected = row == v.selected;
        uint16_t fg = isSelected ? PAPER : INK;
        if (isSelected) canvas->fillRoundRect(MARGIN - 12, y + 4, SW - 2 * MARGIN + 24, ROW_H - 8, 12, INK);

        if (row == 0) drawNewRecordingRow(y, fg, isSelected, v);
        else drawMemoRow(row - 1, y, fg, v);

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

    if (v.memoCount == 0) {
        int y = LIST_TOP + ROW_H + 110;
        drawTextCentered(cx, y, &FreeSansBold12pt7b, "No memos yet", INK);
        drawTextCentered(cx, y + 38, &FreeSans12pt7b, "Click New recording,", INK);
        drawTextCentered(cx, y + 66, &FreeSans12pt7b, "or hold BOOT to record", INK);
    }
}

static void drawRecordingView(const ViewState &v) {
    int cx = SW / 2;
    bool saving = v.audio == AUDIO_SAVING;
    if (saving) canvas->drawCircle(cx, 270, 26, INK);
    else canvas->fillCircle(cx, 270, 26, INK);
    drawTextCentered(cx, 368, &FreeSansBold24pt7b, saving ? "Saving" : "Recording", INK);

    char elapsed[12], limit[12], buf[32];
    formatTime(elapsed, sizeof(elapsed), v.recordSeconds);
    drawTextCentered(cx, 436, &FreeSansBold24pt7b, elapsed, INK);

    // How much of the allowed length is used
    int limitSeconds = max(1, (int)audioRecordingLimitSeconds());
    const int barW = 300, barH = 14, barX = cx - barW / 2, barY = 470;
    canvas->drawRect(barX, barY, barW, barH, INK);
    canvas->drawRect(barX + 1, barY + 1, barW - 2, barH - 2, INK);
    canvas->fillRect(barX + 3, barY + 3, (barW - 6) * min(v.recordSeconds, limitSeconds) / limitSeconds, barH - 6, INK);
    formatTime(limit, sizeof(limit), limitSeconds);
    snprintf(buf, sizeof(buf), "max %s", limit);
    drawTextCentered(cx, barY + 46, &FreeSans12pt7b, buf, INK);
}

// Small modal box over the list: Cancel / Delete, chosen with up/down
static void drawDeleteDialog(const ViewState &v) {
    Memo memo;
    if (!memoGet(v.selected - 1, memo)) return;

    // Shade the list behind it so the dialog stands out
    for (int y = LIST_TOP; y < FOOTER_LINE_Y - 4; y += 2) {
        for (int x = (y / 2 % 2) * 2; x < SW; x += 4) canvas->drawPixel(x, y, INK);
    }

    const int w = SW - 2 * MARGIN - 16, h = 250;
    const int x = (SW - w) / 2, y = 268, cx = SW / 2;
    canvas->fillRoundRect(x - 6, y - 6, w + 12, h + 12, 20, PAPER);
    canvas->fillRoundRect(x, y, w, h, 16, INK);
    canvas->fillRoundRect(x + 3, y + 3, w - 6, h - 6, 13, PAPER);

    iconTrash(cx, y + 58);
    char buf[48], duration[12];
    snprintf(buf, sizeof(buf), "Delete Memo %d?", memo.number);
    drawTextCentered(cx, y + 134, &FreeSansBold18pt7b, buf, INK);
    formatTime(duration, sizeof(duration), memoSeconds(memo));
    snprintf(buf, sizeof(buf), "%s  -  can't be undone", duration);
    drawTextCentered(cx, y + 166, &FreeSans12pt7b, buf, INK);

    const char *labels[2] = {"Cancel", "Delete"};
    const int gap = 20, bw = (w - 3 * gap) / 2, bh = 48, by = y + h - bh - 18;
    for (int i = 0; i < 2; i++) {
        int bx = x + gap + i * (bw + gap);
        bool chosen = i == v.confirmChoice;
        if (chosen) {
            canvas->fillRoundRect(bx, by, bw, bh, 10, INK);
        } else {
            canvas->drawRoundRect(bx, by, bw, bh, 10, INK);
            canvas->drawRoundRect(bx + 1, by + 1, bw - 2, bh - 2, 9, INK);
        }
        drawTextCentered(bx + bw / 2, by + 32, &FreeSansBold12pt7b, labels[i], chosen ? PAPER : INK);
    }
}

static void drawSleepScreen(const ViewState &v) {
    int cx = SW / 2;
    iconMoon(cx - 10, 290, 70);
    iconStar(cx + 78, 236, 12);
    iconStar(cx + 52, 330, 7);
    iconStar(cx - 92, 214, 8);
    drawTextCentered(cx, 448, &FreeSansBold24pt7b, "Sleeping", INK);

    char buf[48];
    const char *where = v.sdCard ? "on SD card" : "in memory";
    if (v.memoCount == 1) snprintf(buf, sizeof(buf), "1 memo %s", where);
    else snprintf(buf, sizeof(buf), "%d memos %s", v.memoCount, where);
    drawTextCentered(cx, 500, &FreeSans12pt7b, buf, INK);
    if (v.battery >= 0) {
        snprintf(buf, sizeof(buf), "Battery %d%%", v.battery);
        drawTextCentered(cx, 532, &FreeSans12pt7b, buf, INK);
    }
}

// Context hints in a 2x2 grid
static void drawFooter(const ViewState &v) {
    struct Hint {
        HintIcon icon;
        const char *label;
    } hints[4];
    int n = 0;
    if (v.sleeping) {
        hints[n++] = {HINT_PWR, "Press to wake"};
    } else if (v.audio == AUDIO_SAVING) {
        // nothing to press while saving
    } else if (v.audio == AUDIO_RECORDING) {
        hints[n++] = {HINT_CLICK, "Stop recording"};
    } else if (v.confirmDelete) {
        hints[n++] = {HINT_UPDOWN, "Choose"};
        hints[n++] = {HINT_CLICK, "Confirm"};
    } else if (v.selected == 0) {
        hints[n++] = {HINT_UPDOWN, "Select"};
        hints[n++] = {HINT_CLICK, "Record"};
        hints[n++] = {HINT_BOOT, "Hold to record"};
        hints[n++] = {HINT_PWR, "Sleep"};
    } else {
        bool playingThis = v.audio == AUDIO_PLAYING && v.playingIndex == v.selected - 1;
        hints[n++] = {HINT_UPDOWN, "Select"};
        hints[n++] = {HINT_CLICK, playingThis ? "Stop" : "Play"};
        hints[n++] = {HINT_HOLD_CLICK, "Hold: delete"};
        hints[n++] = {HINT_BOOT, "Hold to record"};
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
    drawHeader(v);
    if (v.audio == AUDIO_RECORDING || v.audio == AUDIO_SAVING) {
        drawRecordingView(v);
    } else {
        drawList(v);
        if (v.confirmDelete) drawDeleteDialog(v);
    }
    drawFooter(v);
}

// --- Panel output ---

static void pushPartial(const ViewState &v) {
    uint32_t start = millis();
    Serial.printf("[display] %lu ms: refresh (selected %d, memos %d, audio %d, confirm %d)\n",
                  start, v.selected, v.memoCount, v.audio, v.confirmDelete);
    EPD_3IN97_Display_Partial(canvas->getBuffer(), 0, 0, EPD_3IN97_WIDTH, EPD_3IN97_HEIGHT);
    Serial.printf("[display] partial refresh took %lu ms\n", millis() - start);
}

static void pushFull() {
    Serial.printf("[display] %lu ms: full refresh\n", millis());
    EPD_3IN97_Init();
    EPD_3IN97_Display_Base(canvas->getBuffer()); // writes both panel buffers, which partial refreshes build on
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
        ViewState v = currentView();
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
            // leave the sleeping panel alone
        } else if (memcmp(&v, &shown, sizeof(v)) != 0) {
            render(v);
            pushPartial(v);
            shown = v;
            partials++;
        } else if (partials >= FULL_REFRESH_EVERY && v.audio == AUDIO_IDLE && !v.confirmDelete) {
            pushFull();
            partials = 0;
        }
        if (screenshotRequested) {
            screenshotRequested = false;
            dumpScreenshot();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void uiStart() {
    canvas = new GFXcanvas1(EPD_3IN97_WIDTH, EPD_3IN97_HEIGHT);
    canvas->setRotation(SCREEN_ROTATION);
    render(currentView());
    pushFull();
    xTaskCreatePinnedToCore(displayTask, "display", 8192, NULL, 1, NULL, 1);
}

static const char *eventName(ButtonEvent event) {
    switch (event) {
    case BTN_UP: return "up";
    case BTN_DOWN: return "down";
    case BTN_CLICK: return "click";
    case BTN_LONG_CLICK: return "long click";
    case BTN_POWER: return "power";
    }
    return "?";
}

void uiHandleEvent(ButtonEvent event) {
    AudioState audio = audioState();
    Serial.printf("[ui] %lu ms: %s (selected %d, audio %d, confirm %d)\n",
                  millis(), eventName(event), (int)selected, (int)audio, (int)confirmDelete);

    if (event == BTN_POWER) {
        confirmDelete = false;
        sleepRequested = true;
        return;
    }
    if (confirmDelete) {
        switch (event) {
        case BTN_UP:
        case BTN_DOWN:
            confirmChoice = 1 - confirmChoice;
            confirmSince = millis();
            break;
        case BTN_CLICK:
            if (confirmChoice == 1) audioDelete(selected - 1);
            confirmDelete = false;
            break;
        default:
            break;
        }
        return;
    }
    if (audio == AUDIO_RECORDING || audio == AUDIO_SAVING) {
        if (event == BTN_CLICK) audioStopRecording();
        return;
    }

    switch (event) {
    case BTN_UP:
        if (selected > 0) selected = selected - 1;
        break;
    case BTN_DOWN:
        if (selected < memoCount()) selected = selected + 1;
        break;
    case BTN_CLICK:
        if (selected == 0) audioStartRecording();
        else if (audio == AUDIO_PLAYING && audioPlayingIndex() == selected - 1) audioStopPlayback();
        else audioPlay(selected - 1);
        break;
    case BTN_POWER:
        break; // handled above
    case BTN_LONG_CLICK:
        if (selected > 0) {
            if (audio == AUDIO_PLAYING) audioStopPlayback();
            confirmChoice = 0;
            confirmDelete = true;
            confirmSince = millis();
        }
        break;
    }
}

void uiTick() {
    int count = memoCount();
    if (count > lastMemoCount) selected = 1; // a recording just finished: select it so a click plays it
    lastMemoCount = count;
    if (selected > count) selected = count;
    if (confirmDelete && millis() - confirmSince > CONFIRM_TIMEOUT_MS) confirmDelete = false;
}

void uiRequestScreenshot() { screenshotRequested = true; }

bool uiTakeSleepRequest() {
    bool requested = sleepRequested;
    sleepRequested = false;
    return requested;
}

void uiShowSleepScreen(bool on) {
    sleeping = on;
    if (!on) return; // waking happens in the background
    uint32_t start = millis();
    while (!panelAsleep && millis() - start < SLEEP_SCREEN_TIMEOUT_MS) delay(20);
}
