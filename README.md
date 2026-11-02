# Zvukopis

A pocket recorder for sonic moments, built on the Waveshare ESP32-S3-ePaper-3.97
board: a 3.97" e-paper screen, a side rocker switch, a microphone, a speaker,
a microSD slot and a battery, in a 3D-printed case.

> **Warning: experimental firmware.** The idea and concept are
> [Jonáš Gruska](https://lom.audio)'s; the programming was done by Claude
> (Anthropic's AI) in conversation with him. The code was tested on the device
> as it went and never audited. It may lose a recording, drain the battery or
> fail in a way nobody has seen yet. It comes with no warranty of any kind; see
> the licence.

A recording is a *moment*: the sound of here and now. Moments are listed with
the time they were taken, a waveform and a tag, and can be played back,
browsed by day in a calendar, tagged, deleted, analysed (spectrogram and
spectrum), downloaded over Wi-Fi and recorded on a schedule. There is also a
small library: Markdown or text files on the card can be read on the screen.

## Using it

The [user guide](docs/guide.md) covers everything below screen by screen; it
is also on the project page, and it reads on the device (a copy sits in
`library/` next to the sample document). In short:

- **Record:** press the side switch in on *New recording*. Press again to stop.
  With an SD card in, a recording can run for up to 6 hours.
- **Listen:** move to a moment and press in. Holding up or down while it plays
  changes the speed, like a tape reel; the top speed and the ramp are in
  Settings > Varispeed.
- **Tag, analyse, delete:** hold the switch in on a moment.
- **Calendar:** up from *New recording*, then press in.
- **Library:** the book button next to the calendar, shown once the card has
  documents. Up and down turn the pages; the page you left at is remembered.
- **Spectrogram:** up switches between spectrogram and spectrum, down steps
  through four contrast levels.
- **Settings:** the small button next to the USB port (the board calls it BOOT).
  Mic gain (auto or fixed), scheduled recordings,
  the web interface, clock sync, the USB card reader, formatting the card
  and resetting the settings (for handing the device on; the card stays).
- **Quick record:** while asleep, hold the side switch in. The recording
  screen comes up and the recording starts right after it (the panel's
  refresh is audible to the mic, so it goes first; about 3 s from the press).
  A short knock is ignored. Press in again to stop.
- **Which firmware:** the version (date and commit) is at the bottom of
  Settings, and in the web interface's first line. The project page installs
  the newest one.
- **Sleep and wake:** hold the sleep button (PWR on the board) for 1 second.
  Holding it for 10 seconds restarts
  the device. It also goes to sleep by itself after 5 minutes, and wakes for
  scheduled recordings.

### Web interface

*Settings > Web interface* starts a web page on the device. The first time,
with no Wi-Fi network known, Zvukopis opens its own hotspot and shows a QR code:
scan it with a phone camera to join, and the page opens by itself so you can
save your Wi-Fi network. From then on it joins that network and is at
`http://zvukopis.local` (or the address shown on the screen).

The page has a *Recording* tab (record and stop, mic gain, a calendar of the
days with moments, the moments with playback, download and delete) and a
*Settings* tab (scheduled recordings, categories, varispeed, time and time
zone, library, Wi-Fi networks). *Download all* saves every moment as one ZIP of WAV files in
day folders (a backup); a day picked in the calendar can be downloaded on its
own. ZIPs stay under 4 GB, so a bigger collection goes day by day.

### Library

Documents go in `/doc` on the card, uploaded from the web interface or copied
there with the USB card reader. Markdown (`.md`) and plain text (`.txt`) files
up to 512 KB; the first `#` heading is the title. Headings, paragraphs,
`> quotes`, lists, rules, *italic* and **bold** are laid out in Atkinson
Hyperlegible; the text is shown as ASCII, with typographic quotes and dashes
mapped to their plain forms. `library/walden-sounds.md` is a sample, the
chapter *Sounds* from Thoreau's *Walden*.

### SD card

Moments are WAV files (96 kHz, mono, 16-bit) in a folder per day:

    /moments/2026-10-28/2026-10-28T10-12-27_nature.wav

The time uses dashes (FAT names can't hold colons) and the tag goes at the end.
Each file carries a Broadcast Wave `bext` chunk with the time of day the
recording started, so audio editors place it on their timeline.

Cards over 32 GB come formatted as exFAT, which the firmware can't read; it
offers to format such a card (FAT32) in Settings, and the USB card reader
also works with it, so you can format it from a computer instead.

## Technical specification

[docs/tech-spec.md](docs/tech-spec.md): the audio chain, levels and limits of
this board with this firmware, measured where it says so.

## The case

Zvukopis is the Waveshare board with its speaker and battery in a 3D-printed
case. LOM does not sell the board or a kit: you buy the board yourself and
print the case. The cases are in [docs/case/](docs/case/), an STL and an
exploded view per version, with a list of the versions.

What you need:

- The Waveshare [ESP32-S3-ePaper-3.97](https://www.waveshare.com/esp32-s3-epaper-3.97.htm)
  board, from Waveshare or a distributor. Take the version *with lithium
  battery* (or the kit, which adds a card, a cable and a card reader): the
  speaker is in every box, the battery is not. Any other 3.7 V lithium battery
  with an MX1.25 two-pin plug works too.
- The case, 3D-printed from
  [docs/case/zvukopis-case-v1.stl](docs/case/zvukopis-case-v1.stl) (the
  newest; every version is listed in that folder) on an ordinary FDM printer
  or a resin printer. The project page shows it in 3D.
- Four countersunk M2.5×6 bolts, hex (Allen) or Torx, and the key that fits
  them. They go through the case into the board's standoffs.
- Thin double-sided acrylic tape, for attaching the battery to the case.
- A microSD card and a USB-C cable.

Putting it together: peel the backing off the speaker's own tape and stick the
speaker over the speaker holes in the case (the small grille near the top);
make sure the battery cable is long enough to be plugged in to the board from
where the battery will sit, before attaching the battery to the case (there
is usually just one orientation that works), then attach it with the tape,
clear of the bolt holes; plug the battery and the speaker into their
connectors on the board (the plugs fit one way round only); set the case on
the back of the board, the leads tucked so nothing is pinched, and bolt it to
the board's four standoffs, snug rather than tight.

## Flashing

**Without any tools:** open the project page,
[lom-instruments.github.io/zvukopis](https://lom-instruments.github.io/zvukopis/),
in Chrome or Edge, connect the board over USB and press *Install*. After flashing, unplug and replug the USB
cable, or hold the sleep button for 10 seconds: the board stays in download mode after the
flasher's reset.

**From source:** [PlatformIO](https://platformio.org) with the `espressif32`
platform. `pio run -t upload` builds and flashes; `scripts/make_release.py`
builds the `release` environment (the same firmware without any baked-in
Wi-Fi credentials, and it checks the binary for them), copies the binaries
into `docs/firmware/` and writes the flasher's manifest. With `--publish` it
also commits that, pushes, and creates a
[GitHub Release](https://github.com/LOM-instruments/zvukopis/releases) tagged
with the version, the binaries attached and notes made from the commits since
the previous release. The version (date and commit) is what Settings on the
device shows.

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
- `docs/` the web flasher, served by GitHub Pages; `docs/case/` the case,
  an STL and an exploded view per version.

## Licence

MIT, see `LICENSE`. The third-party parts under `lib/` keep their own
licences, listed below.

## Credits

- Idea and concept: Jonáš Gruska ([LOM](https://lom.audio)).
- Programming: Claude, Anthropic's AI, working with him in Claude Code.
- Fonts: [Atkinson Hyperlegible Next and Mono](https://www.brailleinstitute.org/freefont/)
  by the Braille Institute of America, rendered to bitmaps unaltered.
- Sleep-screen quotes: Pauline Oliveros.
- E-paper driver: Waveshare. ES8311 driver: Espressif (Apache 2.0).
  `WebServer`: the Arduino core for ESP32 (LGPL 2.1), two timeouts changed.
- Web flasher: [ESP Web Tools](https://esphome.github.io/esp-web-tools/) (MIT).
