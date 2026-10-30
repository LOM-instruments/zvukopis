# zvukopis: technical specification

The Waveshare ESP32-S3-ePaper-3.97 board running the zvukopis firmware, as a
sound recorder. Figures marked *measured* come from an anechoic room with a
94 dB SPL calibrator on 2026-10-30; the absolute levels are preliminary (the
calibrator was held to the board's microphone pinhole without a sealed
coupler), the relative ones are solid.

## Board

| | |
|---|---|
| Board | Waveshare ESP32-S3-ePaper-3.97 |
| Processor | ESP32-S3R8, dual core, 8 MB octal PSRAM, 16 MB flash |
| Screen | 3.97" e-paper, 480 x 800, partial refresh about 0.55 s, full about 2.2 s |
| Controls | Side rocker switch (up, down, press, hold), Settings button, Sleep button |
| Storage | microSD over SDMMC (4-bit), FAT32 (the firmware formats cards over 32 GB) |
| Power | LiPo battery through an AXP2101 PMIC, USB-C charging, light sleep between presses |
| Radio | Wi-Fi 2.4 GHz (web interface, time sync); off unless started |

## Audio chain

| Stage | |
|---|---|
| Microphone | Analog MEMS capsule (MIC2 on the schematic), mono, behind a pinhole in the board |
| Codec | Everest ES8311 (mono ADC and DAC, 24-bit internally), I2S, master clock 12.288 MHz |
| Analog gain | Codec preamp, 0 to 30 dB in 3 dB steps |
| Digital gain | In the codec after the ADC, 0 to 24 dB in 6 dB steps |
| Mic gain settings | 18 to 54 dB in 6 dB steps, or Auto; 18 to 30 dB are analog only, above that 30 dB analog plus digital |
| Auto gain | Steps down within 16 ms of a peak above -3 dBFS, up after about 1.5 s below -20 dBFS; floor 18 dB, start 36 dB |
| Sample rate | 96 kHz (firmware from 2026-10-30; files from the 48 kHz firmware still load and play) |
| Sample format | 16-bit PCM, mono, samples as the ADC delivers them (no normalisation in the data) |
| Settling | The first 100 ms after the codec wakes are dropped |
| Output | Codec DAC into the on-board speaker; playback with varispeed from -3x to +3x through zero |

## Levels, measured

| | |
|---|---|
| Sensitivity | About -69 dBFS for 94 dB SPL at 1 kHz with 0 dB gain, i.e. -51 dBFS at 18 dB, -27 dBFS at 42 dB (preliminary) |
| Gain accuracy | 6 dB steps within 0.4 dB |
| Noise floor, 0 to 12 dB gain | -92 dBFS unweighted (20 Hz to 20 kHz), -94 dBFS(A): the codec's own floor, independent of the preamp |
| Noise floor, 42 dB gain | -77 dBFS unweighted, -83 dBFS(A) |
| Signal to noise at 94 dB SPL | 25 dB(A) at 0 dB gain, 37 dB(A) at 12 dB, 56 dB(A) at 42 dB |
| Equivalent input noise | At most 38 dB SPL(A) (preliminary upper bound) |
| Clipping | A sine clips at about 121 dB SPL at 42 dB gain, 145 dB SPL at 18 dB |
| 16-bit quantisation | 6 dB below the codec's floor at the lowest gains, so more bits would not add resolution |

Why the gain starts at 18 dB: below it the signal sinks towards the codec's
floor while the headroom it buys (clipping above 145 dB SPL) is of no use.

## Frequency response, measured

| | |
|---|---|
| Bandwidth | Real output to 40 kHz: a resonance peak in the 16 to 20 kHz bands, 16 dB lower at 25 kHz, about 20 dB lower from 32 to 40 kHz (still 35 dB above the floor there) |
| Above 40 kHz | Falls off; the codec's filter ends near 48 kHz |
| Noise floor above 20 kHz | About -100 dBFS per 1/3-octave band at 12 dB gain |
| Reference-microphone response | Not yet measured |

## Recording

| | |
|---|---|
| Longest moment | 6 hours with a card (one WAV file stays under 4 GB); about 30 s in memory without a card |
| File format | WAV, PCM 16-bit mono 96 kHz; a Broadcast Wave `bext` chunk (time of day the recording started as a sample timecode, the mic gain in the description) and a `zvkp` chunk (number, tag, waveform) |
| Files | `/moments/YYYY-MM-DD/YYYY-MM-DDTHH-MM-SS_tag.wav` |
| Card writing | Through a 4 s PSRAM ring, so the card's pauses don't drop samples |
| Quick record | From sleep, about 3 s from the press to the first sample (0.8 s hold, then the panel refresh, which the microphone would otherwise hear) |

## Analysis on the device

| | |
|---|---|
| Spectrogram | 4096-point FFT, Hann window, 23 Hz bins, linear 0 to 48 kHz, four contrast levels |
| Spectrum | Average of the same FFTs, dB against linear frequency |
| Band | The range that holds 80 % of the moment's energy |

## Web interface

| | |
|---|---|
| Address | `http://zvukopis.local` on the joined network, or the device's own hotspot with a QR code |
| Functions | Record and stop, mic gain, calendar of moments, play, download one moment, a day or everything (ZIP, under 4 GB), scheduled recordings, tags, clock and time zone, library documents, Wi-Fi networks |
