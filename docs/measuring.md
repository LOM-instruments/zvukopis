# Measuring the microphone

What zvukopis can tell you about its own microphone and gain chain, given an
anechoic room, a calibrator and a reference microphone. The analysis is
`scripts/measure_mic.py` (Python 3 with numpy); it reads the WAV files the
device writes and knows the mic gain from each file's `bext` description.

## The chain

Analog MEMS microphone, the ES8311 codec's analog preamp (0 to 30 dB in 3 dB
steps), its ADC, then digital gain in the codec (0 to 24 dB in 6 dB steps):

| Mic gain setting | Analog | Digital |
|---|---|---|
| 0 to 30 dB | that | 0 dB |
| 36 dB | 30 dB | 6 dB |
| 42 dB | 30 dB | 12 dB |
| 48 dB | 30 dB | 18 dB |
| 54 dB | 30 dB | 24 dB |

The WAV holds the ADC's samples as they are (16-bit, 48 kHz). The "playback
gain" in the file's `zvkp` chunk is a normalisation the device applies when
it plays; it does not touch the data. Each file's `bext` description says
`mic gain N dB`, with `(auto)` when auto gain was on (then N is the gain at
the end, and it may have changed meanwhile), so measurements need a fixed
gain: set it on the device (Settings > Mic gain) or on the web interface.

## Measurements

Record each with a fixed gain and download the files from the web interface
(a day's ZIP is convenient). Keep the first second quiet: the script skips
half a second, and the hand on the switch is in the recording.

1. **Sensitivity and distortion.** The calibrator at 94 dB SPL on the
   microphone (it sits behind the small hole on the board's edge; a cup with
   a soft seal works), 10 s, at 0, 12 and 24 dB gain. Expect clipping above
   that: 94 dB SPL is about 12 mV at the microphone, and 30 dB of analog gain
   puts it near the ADC's full scale.

       python3 scripts/measure_mic.py tone cal-0dB.wav --spl 94

   Gives the level in dBFS, the sensitivity in dBFS/Pa at that gain and
   referred to 0 dB, THD and THD+N, the harmonics, and the SPL at which a
   sine would clip at that gain.

2. **Noise floor and equivalent input noise.** The room, nothing happening,
   20 s at the same gains, plus 42 dB (the usual auto gain indoors).

       python3 scripts/measure_mic.py noise room-0dB.wav --tone cal-0dB.wav --spl 94

   Gives the floor in dBFS (unweighted and A-weighted), how far above 16-bit
   quantisation it sits (the answer to "would 24 bits help"), the equivalent
   input noise in dB SPL(A), the signal-to-noise ratio at 94 dB SPL and the
   dynamic range to clipping at that gain.

3. **Frequency response.** Pink noise or a long sweep from a speaker in the
   room, recorded by zvukopis and by a reference microphone at the same spot
   (one after the other is fine; 30 s each), at 12 dB gain or whatever stays
   well below clipping.

       python3 scripts/measure_mic.py spectrum pink-zvukopis.wav --ref pink-reference.wav --svg response.svg --csv response.csv

   Prints 1/3-octave band levels for both and the difference, normalised to
   0 dB at 1 kHz: that is zvukopis's response relative to the reference. The
   SVG is a plot of it; `--bands 12` gives 1/12-octave resolution.

4. **Above 20 kHz.** The normal firmware samples at 48 kHz, so the recording
   ends at 24 kHz, and the codec's filter rolls off just below that. For a
   look further up there is a 96 kHz build:

       pio run -e measure96k -t upload

   It records 96 kHz WAVs (48 kHz bandwidth) and skips the 48 kHz moments
   already on the card (they are left alone; put in a spare card to be safe),
   and the 48 kHz firmware skips its files in turn. Record the air compressor,
   then `spectrum` as above: the bands run to 48 kHz. What you see is the
   microphone and the codec together; a MEMS microphone typically has a
   resonance peak somewhere between 15 and 30 kHz and still responds beyond.
   Put the normal firmware back afterwards (`pio run -e esp32s3-epaper -t
   upload`, or the web flasher).

## What to expect

- The microphone's self-noise, not the ADC, sets the floor at every gain
  above about 6 dB: the floor rises with the gain. A typical analog MEMS
  capsule has 60 to 65 dB of signal-to-noise at 94 dB SPL, so the equivalent
  input noise should come out around 30 dB SPL(A).
- At 0 dB gain the floor should land within a few dB of 16-bit quantisation,
  which is where 24 bits would start to matter, and only there.
- Wi-Fi, the e-paper refresh and the USB cable all add noise; measure with
  the web interface off (download afterwards), the screen still (it is,
  while recording) and on battery.
