# zvukopis

A pocket recorder for sonic moments, built on the Waveshare ESP32-S3-ePaper-3.97
board: a 3.97" e-paper screen, a side rocker switch, a microphone, a speaker,
a microSD slot and a battery.

A recording is a *moment*: the sound of here and now. Moments are listed with
the time they were taken, a waveform and a tag, and can be played back,
browsed by day in a calendar, tagged, deleted, analysed (spectrogram and
spectrum), downloaded over Wi-Fi and recorded on a schedule.

## Using it

- **Record:** press the side switch in on *New recording*. Press again to stop.
  With an SD card in, a recording can run for up to 12 hours.
- **Listen:** move to a moment and press in. Holding up or down while it plays
  changes the speed, like a tape reel.
- **Tag, analyse, delete:** hold the switch in on a moment.
- **Calendar:** up from *New recording*, then press in.
- **Settings:** the BOOT button. Mic gain (auto or fixed), scheduled recordings,
  the web interface, clock sync, the USB card reader and formatting the card.
- **Sleep and wake:** hold PWR for 2 seconds. Holding it for 10 seconds restarts
  the device. It also goes to sleep by itself after 5 minutes, and wakes for
  scheduled recordings.

### Web interface

*Settings > Web interface* starts a web page on the device. The first time,
with no Wi-Fi network known, zvukopis opens its own hotspot and shows a QR code:
scan it with a phone camera to join, and the page opens by itself so you can
save your Wi-Fi network. From then on it joins that network and is at
`http://zvukopis.local` (or the address shown on the screen).

The page has a *Recording* tab (record and stop, mic gain, the moments with
playback, download and delete) and a *Settings* tab (scheduled recordings,
categories, time and time zone, Wi-Fi networks).

### SD card

Moments are WAV files (48 kHz, mono, 16-bit) in a folder per day:

    /moments/2026-10-28/2026-10-28T10-12-27_nature.wav

The time uses dashes (FAT names can't hold colons) and the tag goes at the end.
Each file carries a Broadcast Wave `bext` chunk with the time of day the
recording started, so audio editors place it on their timeline.

Cards over 32 GB come formatted as exFAT, which the firmware can't read; it
offers to format such a card (FAT32) in Settings, and the USB card reader
also works with it, so you can format it from a computer instead.

## Flashing

**Without any tools:** open the project page,
[lom-instruments.github.io/zvukopis](https://lom-instruments.github.io/zvukopis/),
in Chrome or Edge, connect the board over USB and press *Install*. After flashing, unplug and replug the USB
cable, or hold PWR for 10 seconds: the board stays in download mode after the
flasher's reset.

**From source:** [PlatformIO](https://platformio.org) with the `espressif32`
platform. `pio run -t upload` builds and flashes; `scripts/make_release.py`
builds the `release` environment (the same firmware without any baked-in
Wi-Fi credentials, and it checks the binary for them), copies the binaries
into `docs/firmware/` and writes the flasher's manifest.

Wi-Fi credentials are not in the code: they are saved from the web interface.
For development, `src/wifi_config.example.h` can be copied to
`src/wifi_config.h` (ignored by git) to bake in a network.

## Layout

- `src/` the firmware: `ui.cpp` (screens, input), `audio.cpp` (codec,
  recording, playback), `storage.cpp` (SD card, WAV files), `transfer.cpp`
  (web interface), `networks.cpp` (Wi-Fi), `clock.cpp` (RTC, NTP, time zone),
  `config.cpp` (categories, schedule), `spectrum.cpp` (FFT analysis),
  `cardreader.cpp` (USB mass storage), `main.cpp` (sleep, scheduler).
- `lib/` the e-paper driver (Waveshare), the ES8311 codec driver (Espressif)
  and a copy of the Arduino `WebServer` library with shorter connection waits.
- `scripts/` tools: `make_fonts.py` renders the UI fonts, `import_quotes.py`
  builds the sleep-screen quotes, `serial_session.py` drives the device over
  USB for testing, `make_release.py` prepares the web flasher.
- `docs/` the web flasher, served by GitHub Pages.

## Licence

MIT, see `LICENSE`. The third-party parts under `lib/` keep their own
licences, listed below.

## Credits

- Fonts: [Atkinson Hyperlegible Next and Mono](https://www.brailleinstitute.org/freefont/)
  by the Braille Institute of America, rendered to bitmaps unaltered.
- Sleep-screen quotes: Pauline Oliveros.
- E-paper driver: Waveshare. ES8311 driver: Espressif (Apache 2.0).
  `WebServer`: the Arduino core for ESP32 (LGPL 2.1), two timeouts changed.
- Web flasher: [ESP Web Tools](https://esphome.github.io/esp-web-tools/) (MIT).
