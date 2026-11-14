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

More changes since, each marked `Zvukopis:` as well. The first three came
with a host closing the audio stream and opening it again:

- **Closing one endpoint** (`dcd_edpt_close`). Upstream has no such function,
  so the stack's close of the stream's endpoint (alternate setting 0) did
  nothing and its bookkeeping for the endpoint never cleared. A first version
  wrote the endpoint's control register to zero, as `dcd_edpt_close_all` does.
  That does not take back a transfer that is armed, and an audio stream always
  has its next packet loaded: the endpoint stayed enabled with the stale
  packet in its FIFO, and since this driver loads a packet only once the FIFO
  is empty, the second open of a stream sent nothing (the microphone's stall
  guard then restarted the device after 3 s). The close now stops the endpoint
  the way the controller asks (`in_ep_stop`): NAK, wait until that holds,
  disable, wait for that, flush the endpoint's FIFO. Every wait gives up after
  about 2 ms, so the USB task cannot hang there. The FIFO-empty interrupt is
  masked first, or the interrupt handler would load the packet again. The FIFO
  number is assigned to the flush register, not OR-ed in, because a bus reset
  leaves "all FIFOs" (0x10) there; and 0x10 is put back when the flush is
  over, because upstream's stall code ORs its own number into that field. The
  endpoint keeps its FIFO for the reopen.
- **Opening an endpoint that is still armed** stops it the same way first. A
  bus reset and a change of configuration do not go through the close, and
  neither takes an armed transfer back.
- **Stalling an armed IN endpoint** (`dcd_edpt_stall`) waited, with no way out,
  for a bit of the control register to show in the interrupt register, where
  it never does. It waits for the "NAK effective" interrupt now, and both of
  its waits are bounded like the ones above.
- **`dcd_sof_enable`** is implemented (upstream left it empty and switched the
  frame interrupt off after each one), and `transmit_packet` never writes more
  than the FIFO has room for.
- The stack's own `tud_suspended()` cannot tell a quiet bus: this driver
  keeps the resume interrupt masked, so the flag, once set, stays set until
  the next bus reset. `src/usbmic.cpp` therefore watches the USB task itself
  (a call queued to it with `usbd_defer_func`), not the bus.
