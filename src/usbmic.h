#pragma once

#include <stddef.h>
#include <stdint.h>

// USB microphone: Zvukopis as a class-compliant USB Audio Class 2.0 microphone
// (mono, 16-bit, SAMPLE_RATE), no driver needed on a computer, phone or tablet.
// The USB port then belongs to the microphone (with a serial port next to it
// for the stream's log), so the console is gone until usbMicLeave(), which
// restarts the board. The audio task feeds the samples (audio.cpp).

bool usbMicStart();        // takes the USB port over; the audio task must be streaming the mic
bool usbMicStreaming();    // the host has the stream open
void usbMicFeed(const int16_t *mono, size_t samples); // from the audio task
void usbMicWatch();        // from the audio task once: it is now the task the watchdog expects to see
void usbMicLeave();        // restarts; does not return
