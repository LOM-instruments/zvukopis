#include "buttons.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_sleep.h>

#include "board_power.h"

// All active low
static const int UP_PIN = 4;
static const int CLICK_PIN = 5;
static const int DOWN_PIN = 6;
static const int KEY_PINS[] = {UP_PIN, CLICK_PIN, DOWN_PIN};

static const uint32_t POLL_MS = 10;
static const uint32_t LONG_PRESS_MS = 800;
static const int POWER_KEY_POLL_EVERY = 10; // polls, so every 100 ms

static QueueHandle_t eventQueue;
static volatile bool paused = false;
static volatile bool keysDown = false; // as the task sees them, after debouncing

struct Key {
    int pin;
    bool down;
    uint8_t disagreeing; // consecutive polls that differ from `down`, for debouncing
    uint32_t pressedAt;
    bool held; // long-press action already fired for this press
};

// Returns +1 on press, -1 on release, 0 otherwise
static int updateKey(Key &key) {
    bool raw = digitalRead(key.pin) == LOW;
    if (raw == key.down) {
        key.disagreeing = 0;
        return 0;
    }
    if (++key.disagreeing < 2) return 0;
    key.disagreeing = 0;
    key.down = raw;
    if (raw) {
        key.pressedAt = millis();
        key.held = false;
        return 1;
    }
    return -1;
}

static void buttonTask(void *) {
    Key up = {UP_PIN}, click = {CLICK_PIN}, down = {DOWN_PIN};
    int polls = 0;

    for (;;) {
        if (paused) {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            continue;
        }
        int upChange = updateKey(up), downChange = updateKey(down);
        if (upChange) buttonsInject(upChange == 1 ? BTN_UP : BTN_UP_RELEASE);
        if (downChange) buttonsInject(downChange == 1 ? BTN_DOWN : BTN_DOWN_RELEASE);
        int clickChange = updateKey(click);
        uint32_t now = millis();

        if (click.down && !click.held && now - click.pressedAt >= LONG_PRESS_MS) {
            click.held = true;
            buttonsInject(BTN_LONG_CLICK);
        }
        if (clickChange == -1 && !click.held) buttonsInject(BTN_CLICK);


        if (++polls % POWER_KEY_POLL_EVERY == 0 && powerKeyPressed()) buttonsInject(BTN_POWER);
        keysDown = up.down || click.down || down.down;

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void buttonsInit() {
    eventQueue = xQueueCreate(16, sizeof(ButtonEvent));
    for (int pin : KEY_PINS) pinMode(pin, INPUT_PULLUP);
    xTaskCreatePinnedToCore(buttonTask, "buttons", 3072, NULL, 3, NULL, 1);
}

bool buttonsNextEvent(ButtonEvent &event) {
    return xQueueReceive(eventQueue, &event, 0) == pdTRUE;
}

void buttonsInject(ButtonEvent event) {
    xQueueSend(eventQueue, &event, 0);
}

bool buttonsIdle() {
    if (keysDown || powerKeyPending()) return false;
    for (int pin : KEY_PINS) {
        if (digitalRead(pin) == LOW) return false;
    }
    return uxQueueMessagesWaiting(eventQueue) == 0;
}

void buttonsWakeup(bool enable) {
    for (int pin : KEY_PINS) {
        if (enable) gpio_wakeup_enable((gpio_num_t)pin, GPIO_INTR_LOW_LEVEL);
        else gpio_wakeup_disable((gpio_num_t)pin);
    }
    if (enable) esp_sleep_enable_gpio_wakeup();
}

void buttonsPause(bool pause) {
    if (!pause) xQueueReset(eventQueue);
    paused = pause;
    if (pause) vTaskDelay(pdMS_TO_TICKS(2 * POLL_MS)); // let a poll in progress finish
}
