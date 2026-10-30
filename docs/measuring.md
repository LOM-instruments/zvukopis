# Measuring the microphone

A session plan for an anechoic room with a 94 dB SPL calibrator and a
reference microphone. It gives sensitivity, distortion, the noise floor and
equivalent input noise, the frequency response, and a look above 20 kHz.
The analysis is `scripts/measure_mic.py` (Python 3 with numpy); it reads the
WAV files the device writes and learns the mic gain from each file.

## What is being measured

Analog MEMS microphone, the ES8311 codec's analog preamp (0 to 30 dB in
3 dB steps), its ADC, then digital gain in the codec (0 to 24 dB in 6 dB
steps). The mic gain setting maps like this:

| Setting | Analog | Digital |
|---|---|---|
| 0 to 30 dB | that | 0 dB |
| 36 dB | 30 dB | 6 dB |
| 42 dB | 30 dB | 12 dB |
| 48 dB | 30 dB | 18 dB |
| 54 dB | 30 dB | 24 dB |

The WAV holds the ADC's samples as they are (16-bit, 48 kHz; nothing is
normalised in the data). Each file's `bext` description says
`mic gain N dB`, and `(auto)` if auto gain was on, which must not be the
case for a measurement.

## Before entering the room

1. Charge zvukopis. Measure on battery, USB unplugged: the cable and the
   charger add noise.
2. Firmware 2026-10-30-4e2595c or newer (the project page installs it). It
   is what writes the gain into the files.
3. Check the clock on the screen. The files are named by time, which is how
   you tell them apart afterwards.
4. A laptop with the repository and PlatformIO, for the 96 kHz build at the
   end (step 11), and to copy the files off (step 13). No Wi-Fi is needed
   for any of this.
5. Find the microphone: a pinhole in the board, by the audio codec. Start a
   recording and brush a finger near the candidates; the one that roars is
   it. Note where it is; the calibrator and the reference microphone both
   go there.
6. Decide how the 94 dB gets to that pinhole. A calibrator's coupler won't
   seal on a bare board. Either make a soft adapter (a short rubber tube or
   a cup with putty around the pinhole), or do what works in an anechoic
   room anyway: play 1 kHz from the room's speaker, measure the SPL at the
   spot with the reference microphone, then put zvukopis's pinhole at that
   spot and use that SPL with `--spl`. The second way also gives distortion
   at higher levels.

## Setting the gain on the device

Settings button (next to the USB port) > *Mic gain* > press in, choose with
up and down, press in. The choices are Auto, then 0 to 54 dB in 6 dB steps.
Never Auto for a measurement. The gain stays until you change it, and goes
into every file.

## Recording in the room

Each recording: press in on *New recording*, keep still, press in again.
The analysis skips the first second (the press, the screen's refresh), so
hands off straight away and give it the full time. Write down the time of
day and what it was; the file name has the time and the gain is inside.

7. **Calibrator tone, three gains.** The tone on the pinhole, 15 s each, at
   **0 dB**, **12 dB** and **24 dB**. Don't go higher: 94 dB SPL is about
   12 mV at the microphone, and 30 dB of gain puts it near the ADC's full
   scale. If you can play louder tones through the speaker (104, 114 dB SPL,
   measured with the reference mic), record those too at 0 dB: they give
   distortion at level and the clipping point directly.
8. **Silence, four gains.** The room, nobody moving, 30 s each, at **0 dB**,
   **12 dB**, **24 dB** and **42 dB** (42 is what auto gain settles to
   indoors; the comparison between gains shows what sets the floor).
9. **Pink noise (or a slow sweep) for the response.** From the room's
   speaker, zvukopis at a marked spot facing it, **12 dB**, 30 s. Then the
   reference microphone at the same spot, same source, exported as WAV
   (any rate or bit depth; 48 kHz is simplest). If the reference system
   gives you the speaker's own response instead, that works too: what the
   script computes is zvukopis relative to whatever `--ref` is.
10. **Orientation, optional.** The same pink noise with zvukopis turned
    90 and 180 degrees from the speaker, 20 s each, to see how much the
    board and case shade the microphone.

## Above 20 kHz

11. On the laptop, in the repository:

        pio run -e measure96k -t upload

    The device restarts sampling at 96 kHz. It shows 0 moments (the 48 kHz
    files are skipped, not touched). Set the gain again if it asks; it is
    kept in settings, so it should still be what you chose.
12. Record, at **12 dB**, 20 s each: the air compressor (or any broadband
    ultrasonic source: compressed air from a nozzle, keys jangling), the
    same source with the reference microphone if it runs at 96 kHz, and
    30 s of the room's silence (the floor above 20 kHz). Then put the
    normal firmware back:

        pio run -e esp32s3-epaper -t upload

    The 96 kHz files stay on the card, in the day's folder with the others;
    the normal firmware lists only the 48 kHz ones.

## Getting the files

13. Settings > *USB card reader* with the cable in the laptop. Copy the
    day's folder from `moments/`. Eject; the device restarts by itself.
    (The web interface's ZIP works too for the 48 kHz files, but it needs
    Wi-Fi.)

## The analysis

With the files in one folder, from the repository:

    python3 scripts/measure_mic.py info *.wav

lists each file's time, length, gain and peak, so you can match them to
your notes. Then:

    python3 scripts/measure_mic.py tone tone-0dB.wav --spl 94
    python3 scripts/measure_mic.py tone tone-12dB.wav --spl 94
    python3 scripts/measure_mic.py tone tone-24dB.wav --spl 94

Level in dBFS, sensitivity in dBFS/Pa at that gain and referred to 0 dB
(the three should agree; the gain steps should be 12 dB apart), THD, THD+N,
the harmonics, and the SPL at which a sine clips at that gain. A louder
tone from the speaker: `--spl 104`.

    python3 scripts/measure_mic.py noise silence-0dB.wav --tone tone-0dB.wav --spl 94
    python3 scripts/measure_mic.py noise silence-42dB.wav

The floor in dBFS unweighted and A-weighted, how far it sits above 16-bit
quantisation, and with the tone of the same gain: the equivalent input
noise in dB SPL(A), signal-to-noise at 94 dB SPL, and the dynamic range to
clipping. The 0 dB figure is the one that answers "would 24 bits help".

    python3 scripts/measure_mic.py spectrum pink-zvukopis.wav --ref pink-reference.wav --svg response.svg --csv response.csv

1/3-octave levels for both and zvukopis relative to the reference,
normalised to 0 dB at 1 kHz; the SVG plots it. `--bands 12` for
1/12-octave.

    python3 scripts/measure_mic.py spectrum compressor-96k.wav --svg ultrasonic.svg
    python3 scripts/measure_mic.py spectrum silence-96k.wav --svg floor-96k.svg

The bands run to 48 kHz. Where the compressor's spectrum stays well above
the silence's, the microphone and codec are still delivering. A MEMS
capsule usually shows a resonance peak somewhere between 15 and 30 kHz and
a response that carries on above it; the codec's filter at 96 kHz rolls off
near 48 kHz.

## Then

Whether the main firmware moves to 96 kHz is decided on that last plot. It
would halve the recording time per card and make the files twice the size,
so it needs the microphone to deliver something real up there.
