# tinyusb_audio

TinyUSB's USB Audio Class device driver, `audio_device.c`, copied from
[TinyUSB 0.16.0](https://github.com/hathach/tinyusb/tree/0.16.0/src/class/audio)
(MIT, see LICENSE), the version built into the Arduino core this firmware
uses. The core ships TinyUSB prebuilt without the audio class, but with the
audio headers and a hook for extra class drivers, which `src/usbmic.cpp`
uses (the hook is a weak reference, so its definition must be in the app, not
in a library archive). The driver's settings (`CFG_TUD_AUDIO_*`) live in `platformio.ini`.

One change to the upstream file, marked `Zvukopis:` in a comment: when the
host never sets the sample rate (Windows does not, for a device that offers
only one), the rate from `CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE_TX` is used, so
the flow control that keeps the stream steady has something to work from.

The microphone itself is `src/usbmic.cpp`.

## dcd_esp32sx.c

The ESP32-S2/S3 low-level driver, also from TinyUSB 0.16.0
(`src/portable/espressif/esp32sx/dcd_esp32sx.c`), with one change, marked
`Zvukopis:`: the transmit FIFO RAM is handed out by each endpoint's packet
size instead of in equal 188-byte slices, and configured in the register of
the FIFO the endpoint actually uses. The core's copy could not hold a 194-byte
isochronous packet, and the interrupt handler then waited forever for the
host to drain a FIFO it had overfilled, freezing the chip. `src/usbmic.cpp`
references `tinyusbDcdPatched()` so this copy is linked instead of the one in
the prebuilt library (the card reader then uses it too).
