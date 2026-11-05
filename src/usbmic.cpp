#include "usbmic.h"

#include <Arduino.h>
#include <USB.h>
#include <USBCDC.h>
#include <esp32-hal-tinyusb.h>

#include <esp_task_wdt.h>
#include <stdarg.h>

#include <driver/i2s.h>

#include "tinyusb_audio.h"
#include "audio.h"
#include "buttons.h"
#include "usbport.h"

// One mono 16-bit stream at the recorder's rate. Full speed USB carries one
// packet a millisecond, a sample's worth bigger than the nominal 96 (or 48),
// which is how the stream keeps step with the host: TinyUSB's flow control
// sends a sample more or less when its buffer drifts from half full
static const uint16_t EP_SIZE = TUD_AUDIO_EP_SIZE(SAMPLE_RATE, 2, 1);
static_assert(EP_SIZE <= CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX, "platformio.ini: CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX is too small for the sample rate");
static_assert(CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE_TX == SAMPLE_RATE, "platformio.ini: CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE_TX must be the sample rate");
static const size_t PREFILL_BYTES = CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ / 2; // silence, so the stream starts at the flow control's target

// The serial port next to the microphone, for the stream's log and a few
// test commands. The core's USBCDC object puts the port into the descriptor
// and queues what arrives; its write() is never used, because it spins until
// the host has taken what is in the 64-byte USB buffer, which can be never
// (a port left open and unread, or a write from inside a TinyUSB callback,
// which runs on the USB task that would have to do the sending). Instead
// the audio task hands the log to TinyUSB a piece at a time, from a ring of
// its own, and drops what the host does not take.
static USBCDC cdc(0);
static char logRing[2048];
static volatile uint16_t logWr = 0, logRd = 0;

static void logWrite(const char *text) {
    for (; *text; text++) {
        uint16_t next = (logWr + 1) % sizeof(logRing);
        if (next == logRd) return; // full: the rest of the line is lost
        logRing[logWr] = *text;
        logWr = next;
    }
}

static void logPrintf(const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    logWrite(line);
}

// What fits into the USB buffer now, without waiting; called from the audio task
static void logPump() {
    if (!tud_cdc_connected()) {
        logRd = logWr; // nobody listening: forget it
        return;
    }
    uint32_t room = tud_cdc_write_available();
    while (room && logRd != logWr) {
        uint16_t run = logRd < logWr ? logWr - logRd : sizeof(logRing) - logRd;
        if (run > room) run = room;
        uint32_t n = tud_cdc_write(logRing + logRd, run);
        if (!n) break;
        logRd = (logRd + n) % sizeof(logRing);
        room -= n;
    }
    tud_cdc_write_flush();
}
static volatile bool streaming = false;
static volatile bool muted = false;
static int16_t hostVolume = 0;   // in 1/256 dB; taken, but the gain is Zvukopis's own
static uint32_t sampleRate = SAMPLE_RATE;
static uint8_t clockValid = 1;
// For the log: samples fed and sent since the stream opened, and how far the
// driver's buffer swung
static volatile uint32_t fedBytes = 0, sentBytes = 0, droppedSamples = 0, emptyPackets = 0;
static volatile int32_t levelLow = INT32_MAX, levelHigh = 0;
static volatile uint32_t opens = 0, closes = 0, mutes = 0, loggedOpens = 0, loggedCloses = 0, loggedMutes = 0;
static volatile uint32_t prefilledBytes = 0, fifoDepth = 0, fifoAfterPrefill = 0;
static uint32_t openedAt = 0, loggedAt = 0;
// A trace of the first packets after the stream opens: the driver's buffer
// count before the load, the bytes loaded, the count after
static const int TRACE = 24;
static volatile uint16_t trace[TRACE][3];
static volatile uint8_t traceN = 0, tracePrinted = 0;

// The audio function: an input terminal (1), a feature unit with mute and
// volume (2), the USB output terminal (3) and a fixed clock (4)
static uint16_t audioDescriptor(uint8_t *dst, uint8_t *itf) {
    uint8_t ep = tinyusb_get_free_in_endpoint();
    uint8_t str = tinyusb_add_string_descriptor("Zvukopis microphone");
    uint8_t descriptor[TUD_AUDIO_MIC_ONE_CH_DESC_LEN] = {
        TUD_AUDIO_MIC_ONE_CH_DESCRIPTOR(*itf, str, 2, 16, (uint8_t)(0x80 | ep), EP_SIZE)};
    *itf += 2; // the control and the streaming interface
    memcpy(dst, descriptor, sizeof(descriptor));
    return sizeof(descriptor);
}

extern "C" void tinyusbDcdPatched(void); // lib/tinyusb_audio/src/dcd_esp32sx.c

bool usbMicStart() {
    tinyusbDcdPatched(); // links the patched USB driver (its comment says why)
    Serial.println("[usb mic] switching USB to a microphone; serial ends here");
    Serial.flush();
    delay(50);
    if (tinyusb_enable_interface(USB_INTERFACE_CUSTOM, TUD_AUDIO_MIC_ONE_CH_DESC_LEN, audioDescriptor) != ESP_OK) return false;
    cdc.begin(); // a serial port next to the microphone, for the stream's log
    USB.productName("Zvukopis microphone");
    USB.manufacturerName("Zvukopis");
    USB.usbAttributes(TUSB_DESC_CONFIG_ATT_SELF_POWERED); // the battery runs it...
    USB.usbPower(100);                                    // ...and the host is asked for what a tablet allows
    if (!USB.begin()) return false; // takes the USB port over from the serial/JTAG controller
    // Should the audio task stall, the task watchdog panics; should the USB
    // task stall while the host streams, the audio task aborts (log()). Either
    // way a core dump lands in flash (scripts/coredump.py reads it) and the
    // restart brings the console back. (The USB task cannot be watched by
    // the frame tick: the stack switches that interrupt off on a bus reset.)
    esp_task_wdt_init(8, true);
    return true;
}

void usbMicWatch() { esp_task_wdt_add(nullptr); } // the audio task, once it streams

bool usbMicStreaming() { return streaming; }

void usbMicLeave() { usbPortRestart(); }

static volatile uint32_t packets = 0; // loads of the IN endpoint, on the USB task
static uint32_t feeds = 0, feedSamples = 0, feedGapMaxUs = 0, lastFeedUs = 0; // the audio task's chunks

static void log() {
    uint32_t now = millis();
    esp_task_wdt_reset();
    logPump();
    static uint32_t packetsSeen = 0, packetsMovedAt = 0;
    if (!streaming || packets != packetsSeen) {
        packetsSeen = packets;
        packetsMovedAt = now;
    } else if (now - packetsMovedAt > 3000) {
        abort(); // the USB task has stopped loading packets: a core dump says where it is
    }
    if (loggedOpens != opens) {
        loggedOpens = opens;
        logPrintf("[usb mic] stream opened: %lu Hz, 16-bit, mono; fifo %lu bytes, %lu after the prefill of %lu\n", sampleRate, fifoDepth, fifoAfterPrefill, prefilledBytes);
    }
    if (loggedCloses != closes) {
        loggedCloses = closes;
        logWrite("[usb mic] stream closed\n");
    }
    if (tracePrinted != traceN && (!streaming || traceN == TRACE)) {
        tu_fifo_t *ff = tud_audio_n_get_ep_in_ff(0);
        logPrintf("[usb mic] packets (count before, bytes, count after): ");
        for (int i = 0; i < traceN; i++) logPrintf("%u/%u/%u ", trace[i][0], trace[i][1], trace[i][2]);
        logPrintf("; fifo wr %u rd %u depth %u\n", ff ? ff->wr_idx : 0, ff ? ff->rd_idx : 0, ff ? ff->depth : 0);
        tracePrinted = traceN;
    }
    if (loggedMutes != mutes) {
        loggedMutes = mutes;
        logPrintf("[usb mic] host %s\n", muted ? "muted" : "unmuted");
    }
    if (now - loggedAt < 5000) return;
    static uint32_t packetsAtLog = 0;
    uint32_t interval = now - loggedAt;
    loggedAt = now;
    logPrintf("[usb mic] %lu s: %s, gain %d dB%s, fed %lu, sent %lu, dropped %lu samples, empty packets %lu, buffer %ld..%ld bytes; "
              "in %lu ms: %lu chunks, %lu samples, chunk gap up to %lu us, %lu packets; i2s clock %.0f Hz\n",
               (now - openedAt) / 1000, streaming ? "streaming" : "idle", audioMicGain(),
               audioMicGainSetting() == GAIN_AUTO ? " (auto)" : "", fedBytes / 2, sentBytes / 2, droppedSamples, emptyPackets,
               levelLow == INT32_MAX ? 0 : levelLow, levelHigh,
               interval, feeds, feedSamples, feedGapMaxUs, packets - packetsAtLog, i2s_get_clk(I2S_NUM_0));
    packetsAtLog = packets;
    feeds = feedSamples = feedGapMaxUs = 0;
    levelLow = INT32_MAX;
    levelHigh = 0;
}

void usbMicFeed(const int16_t *mono, size_t samples) {
    uint32_t us = micros();
    if (lastFeedUs && us - lastFeedUs > feedGapMaxUs) feedGapMaxUs = us - lastFeedUs;
    lastFeedUs = us;
    feeds++;
    feedSamples += samples;
    if (cdc.available()) { // the serial port next to the microphone, for scripted tests: one key per chunk
        switch (cdc.read()) {
        case 'q': usbMicLeave(); break;
        case 'u': buttonsInject(BTN_UP); break;
        case 'd': buttonsInject(BTN_DOWN); break;
        }
    }
    if (!streaming) {
        log();
        return;
    }
    static int16_t silence[256];
    uint16_t written = 0;
    if (muted) {
        for (size_t at = 0; at < samples; at += 256) {
            written += tud_audio_write(silence, min(samples - at, (size_t)256) * 2);
        }
    } else {
        written = tud_audio_write(mono, samples * 2);
    }
    fedBytes += written;
    droppedSamples += samples - written / 2;
    log();
}

extern "C" {

// The audio class driver, handed to the core's prebuilt TinyUSB stack, which
// was built without it; this hook must be in the app, not in a library
static const usbd_class_driver_t audioDriver = {
#if CFG_TUSB_DEBUG >= CFG_TUD_LOG_LEVEL
    .name = "AUDIO",
#endif
    .init = audiod_init,
    .reset = audiod_reset,
    .open = audiod_open,
    .control_xfer_cb = audiod_control_xfer_cb,
    .xfer_cb = audiod_xfer_cb,
    .sof = NULL, // only feedback endpoints need the frame tick
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &audioDriver;
}

// The host opens (alternate setting 1) or closes (0) the stream
bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *request) {
    uint8_t alt = TU_U16_LOW(request->wValue);
    if (alt != 0 && !streaming) {
        tu_fifo_t *ff = tud_audio_n_get_ep_in_ff(0);
        fifoDepth = ff ? ff->depth : 0;
        tud_audio_clear_ep_in_ff();
        static int16_t silence[256];
        size_t prefilled = 0;
        for (size_t at = 0; at < PREFILL_BYTES; at += sizeof(silence)) {
            prefilled += tud_audio_write(silence, min(PREFILL_BYTES - at, sizeof(silence)));
        }
        prefilledBytes = prefilled;
        fifoAfterPrefill = ff ? tu_fifo_count(ff) : 0;
        fedBytes = sentBytes = droppedSamples = emptyPackets = 0;
        levelLow = INT32_MAX;
        levelHigh = 0;
        openedAt = millis();
        traceN = tracePrinted = 0;
        opens++;
    } else if (alt == 0 && streaming) {
        closes++;
    }
    streaming = alt != 0;
    return true;
}

bool tud_audio_set_itf_close_EP_cb(uint8_t rhport, tusb_control_request_t const *request) {
    if (streaming) closes++;
    streaming = false;
    return true;
}

bool tud_audio_tx_done_pre_load_cb(uint8_t rhport, uint8_t func_id, uint8_t ep_in, uint8_t cur_alt_setting) {
    packets++;
    if (traceN < TRACE) {
        tu_fifo_t *ff = tud_audio_n_get_ep_in_ff(0);
        trace[traceN][0] = ff ? tu_fifo_count(ff) : 0;
    }
    return true;
}

// After each packet: how full the driver's buffer is, for the log
bool tud_audio_tx_done_post_load_cb(uint8_t rhport, uint16_t n_bytes_copied, uint8_t func_id, uint8_t ep_in, uint8_t cur_alt_setting) {
    sentBytes += n_bytes_copied;
    if (n_bytes_copied == 0) emptyPackets++;
    if (traceN < TRACE) {
        tu_fifo_t *ff = tud_audio_n_get_ep_in_ff(0);
        trace[traceN][1] = n_bytes_copied;
        trace[traceN][2] = ff ? tu_fifo_count(ff) : 0;
        traceN++;
    }
    int32_t level = (int32_t)(PREFILL_BYTES + fedBytes - sentBytes);
    if (level < levelLow) levelLow = level;
    if (level > levelHigh) levelHigh = level;
    return true;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *request, uint8_t *buf) {
    uint8_t ctrl = TU_U16_HIGH(request->wValue), entity = TU_U16_HIGH(request->wIndex);
    TU_VERIFY(request->bRequest == AUDIO_CS_REQ_CUR);
    if (entity == 2) {
        if (ctrl == AUDIO_FU_CTRL_MUTE) {
            TU_VERIFY(request->wLength == sizeof(audio_control_cur_1_t));
            muted = ((audio_control_cur_1_t *)buf)->bCur != 0;
            mutes++;
            return true;
        }
        if (ctrl == AUDIO_FU_CTRL_VOLUME) {
            TU_VERIFY(request->wLength == sizeof(audio_control_cur_2_t));
            hostVolume = ((audio_control_cur_2_t *)buf)->bCur;
            return true;
        }
    }
    return false;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *request) {
    uint8_t ctrl = TU_U16_HIGH(request->wValue), entity = TU_U16_HIGH(request->wIndex);
    if (entity == 1 && ctrl == AUDIO_TE_CTRL_CONNECTOR) {
        audio_desc_channel_cluster_t ret = {.bNrChannels = 1, .bmChannelConfig = (audio_channel_config_t)0, .iChannelNames = 0};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &ret, sizeof(ret));
    }
    if (entity == 2) {
        if (ctrl == AUDIO_FU_CTRL_MUTE) {
            audio_control_cur_1_t ret = {.bCur = muted};
            return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &ret, sizeof(ret));
        }
        if (ctrl == AUDIO_FU_CTRL_VOLUME) {
            if (request->bRequest == AUDIO_CS_REQ_CUR) {
                audio_control_cur_2_t ret = {.bCur = hostVolume};
                return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &ret, sizeof(ret));
            }
            if (request->bRequest == AUDIO_CS_REQ_RANGE) {
                // One value: the gain is set on Zvukopis, not on the host
                audio_control_range_2_n_t(1) ret;
                ret.wNumSubRanges = 1;
                ret.subrange[0].bMin = 0;
                ret.subrange[0].bMax = 0;
                ret.subrange[0].bRes = 256;
                return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &ret, sizeof(ret));
            }
        }
    }
    if (entity == 4) {
        if (ctrl == AUDIO_CS_CTRL_SAM_FREQ) {
            if (request->bRequest == AUDIO_CS_REQ_CUR) return tud_control_xfer(rhport, request, &sampleRate, sizeof(sampleRate));
            if (request->bRequest == AUDIO_CS_REQ_RANGE) {
                audio_control_range_4_n_t(1) ret;
                ret.wNumSubRanges = 1;
                ret.subrange[0].bMin = SAMPLE_RATE;
                ret.subrange[0].bMax = SAMPLE_RATE;
                ret.subrange[0].bRes = 0;
                return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &ret, sizeof(ret));
            }
        }
        if (ctrl == AUDIO_CS_CTRL_CLK_VALID) return tud_control_xfer(rhport, request, &clockValid, sizeof(clockValid));
    }
    return false;
}

} // extern "C"
