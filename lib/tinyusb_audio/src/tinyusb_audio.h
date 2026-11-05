#pragma once

// TinyUSB's USB Audio Class 2.0 device driver (src/audio_device.c, from
// TinyUSB 0.16.0, the version inside the Arduino core), compiled here because
// the core's prebuilt TinyUSB leaves the audio class out. Including this
// header pulls the library into the build; src/usbmic.cpp registers the class
// driver with the core's USB stack (usbd_app_driver_get_cb, which has to live
// in the app: a definition in a library archive is never linked, since the
// stack only refers to it weakly). The CFG_TUD_AUDIO_* settings are in
// platformio.ini, so that the app and the driver agree.

#include "tusb.h"
#include "class/audio/audio_device.h"
#include "device/usbd_pvt.h"
