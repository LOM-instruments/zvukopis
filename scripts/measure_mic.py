#!/usr/bin/env python3
"""Microphone measurements from zvukopis recordings.

Record on zvukopis with a FIXED mic gain (Settings > Mic gain on the device, or
the Recording tab of the web interface; not Auto), download the WAV files, then:

  tone      a calibrator tone at a known SPL (94 dB SPL = 1 Pa, usually 1 kHz):
            level in dBFS, THD, THD+N, sensitivity in dBFS/Pa, headroom to clipping
  noise     a recording of nothing (anechoic room, same gain as the tone):
            noise floor in dBFS, unweighted 20 Hz-20 kHz and A-weighted; with --tone
            also the equivalent input noise in dB SPL(A), signal-to-noise ratio and
            the dynamic range up to clipping
  spectrum  the averaged spectrum of a recording (air compressor, pink noise, a
            sweep): 1/3-octave band levels up to Nyquist, as a table, a CSV and an
            SVG plot; with --ref a reference microphone's recording of the same
            source, the device's response relative to it (0 dB at 1 kHz)
  info      what a file says about itself: sample rate, length, mic gain, peak

    python3 scripts/measure_mic.py tone cal94.wav --spl 94
    python3 scripts/measure_mic.py noise silence.wav --tone cal94.wav --spl 94
    python3 scripts/measure_mic.py spectrum compressor.wav --ref compressor-ref.wav --svg response.svg
    python3 scripts/measure_mic.py info *.wav

Levels are dBFS with a full-scale sine at 0 dBFS (AES17), so a full-scale square
wave reads +3 dBFS. The first half second of every file is skipped (the codec
settling, the hand on the switch). Needs numpy only.
"""

import argparse
import math
import re
import struct
import sys

import numpy as np

SKIP_SECONDS = 0.5
LOW_HZ, HIGH_HZ = 20.0, 20000.0


# --- files ---

def read_wav(path):
    """Samples as float in -1..1, the sample rate, and the bext description."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        sys.exit(f"{path}: not a WAV file")
    pos, fmt, description, samples = 12, None, "", None
    while pos + 8 <= len(data):
        chunk_id, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if chunk_id == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif chunk_id == b"bext":
            description = body[:256].split(b"\0", 1)[0].decode("latin-1")
        elif chunk_id == b"data":
            samples = body
        pos += 8 + size + (size & 1)
    if fmt is None or samples is None:
        sys.exit(f"{path}: no fmt or data chunk")
    channels, rate, bits = fmt[1], fmt[2], fmt[5]
    if bits == 16:
        x = np.frombuffer(samples, dtype="<i2").astype(np.float64) / 32768.0
    elif bits == 24:
        raw = np.frombuffer(samples, dtype=np.uint8).reshape(-1, 3)
        x = (raw[:, 0].astype(np.int32) | raw[:, 1].astype(np.int32) << 8 | raw[:, 2].astype(np.int8).astype(np.int32) << 16) / 8388608.0
    elif bits == 32:
        x = np.frombuffer(samples, dtype="<i4").astype(np.float64) / 2147483648.0
    else:
        sys.exit(f"{path}: {bits}-bit samples aren't handled")
    if channels > 1:
        x = x.reshape(-1, channels)[:, 0]
    return x, rate, description


def mic_gain(description):
    m = re.search(r"mic gain (\d+) dB( \(auto\))?", description)
    if not m:
        return None, False
    return int(m.group(1)), bool(m.group(2))


def trim(x, rate):
    skip = int(SKIP_SECONDS * rate)
    return x[skip:] if len(x) > 2 * skip else x


# --- maths ---

def db(power):
    return 10 * math.log10(max(power, 1e-30))


def dbfs_from_rms(rms):
    """AES17: a full-scale sine (RMS 1/sqrt 2) is 0 dBFS."""
    return 20 * math.log10(max(rms * math.sqrt(2), 1e-15))


def a_weighting_db(f):
    f = np.asarray(f, dtype=np.float64)
    f2 = f * f
    num = 12194.0 ** 2 * f2 * f2
    den = (f2 + 20.6 ** 2) * np.sqrt((f2 + 107.7 ** 2) * (f2 + 737.9 ** 2)) * (f2 + 12194.0 ** 2)
    with np.errstate(divide="ignore"):
        return 20 * np.log10(num / den) + 2.0


def welch(x, rate, segment=8192):
    """Averaged one-sided power spectrum (Hann, 50% overlap), as power per bin
    scaled so that the sum over bins is the mean square of the signal."""
    if len(x) < segment:
        segment = 1 << (len(x).bit_length() - 1)
    window = np.hanning(segment)
    scale = 1.0 / (np.sum(window ** 2) * segment / 2)  # one-sided, mean square
    hop = segment // 2
    acc, n = np.zeros(segment // 2 + 1), 0
    for start in range(0, len(x) - segment + 1, hop):
        spec = np.fft.rfft(x[start:start + segment] * window)
        acc += np.abs(spec) ** 2
        n += 1
    power = acc / max(n, 1) * scale * 2
    power[0] /= 2
    power[-1] /= 2
    return np.fft.rfftfreq(segment, 1.0 / rate), power


def band_power(freqs, power, lo, hi):
    return float(np.sum(power[(freqs >= lo) & (freqs < hi)]))


def third_octave_bands(fraction, fmax):
    """Centre frequencies and edges of 1/fraction-octave bands from 20 Hz to fmax."""
    bands = []
    n = -10 * fraction // 3  # start below 20 Hz, step to it
    while True:
        centre = 1000.0 * 2 ** (n / fraction)
        n += 1
        if centre < 19.0:
            continue
        lo, hi = centre * 2 ** (-0.5 / fraction), centre * 2 ** (0.5 / fraction)
        if lo >= fmax:
            break
        bands.append((centre, lo, min(hi, fmax)))
    return bands


# --- commands ---

def cmd_info(args):
    for path in args.files:
        x, rate, description = read_wav(path)
        gain, auto = mic_gain(description)
        peak = float(np.max(np.abs(x))) if len(x) else 0
        clipped = int(np.sum(np.abs(x) >= 32767 / 32768))
        rms = float(np.sqrt(np.mean(trim(x, rate) ** 2))) if len(x) else 0
        print(f"{path}")
        print(f"  {rate} Hz, {len(x) / rate:.2f} s, {description!r}")
        print(f"  mic gain {'unknown' if gain is None else f'{gain} dB' + (' (auto, at the end)' if auto else '')}")
        print(f"  RMS {dbfs_from_rms(rms):.1f} dBFS, peak {20 * math.log10(max(peak, 1e-9)):.1f} dBFS, {clipped} clipped sample{'s' if clipped != 1 else ''}")


def analyse_tone(path, freq_hint):
    x, rate, description = read_wav(path)
    x = trim(x, rate)
    freqs, power = welch(x, rate, 16384)
    bin_hz = freqs[1]
    # The fundamental: the strongest bin near the hint (or anywhere above 50 Hz)
    search = (freqs > 50) if freq_hint is None else (np.abs(freqs - freq_hint) < freq_hint * 0.2)
    k = int(np.argmax(np.where(search, power, 0)))
    f0 = freqs[k]
    width = max(3, int(round(f0 * 0.02 / bin_hz)))  # +-2 % around each harmonic

    def around(f):
        return band_power(freqs, power, f - width * bin_hz, f + width * bin_hz)

    fund = around(f0)
    harmonics = [around(h * f0) for h in range(2, 11) if h * f0 < rate / 2]
    total = band_power(freqs, power, LOW_HZ, min(HIGH_HZ, rate / 2))
    thd = math.sqrt(sum(harmonics) / fund) if fund > 0 else 0
    thdn = math.sqrt(max(total - fund, 0) / fund) if fund > 0 else 0
    level = db(fund) + 3.0  # power of a sine of RMS r is r^2; 0 dBFS sine has r^2 = 1/2
    peak = float(np.max(np.abs(x)))
    clipped = int(np.sum(np.abs(x) >= 32767 / 32768))
    return {
        "rate": rate, "description": description, "f0": f0, "level_dbfs": level,
        "thd_pct": thd * 100, "thdn_pct": thdn * 100, "thd_db": 20 * math.log10(max(thd, 1e-9)),
        "peak_dbfs": 20 * math.log10(max(peak, 1e-9)), "clipped": clipped,
        "harmonics_db": [db(h) + 3.0 for h in harmonics],
    }


def cmd_tone(args):
    t = analyse_tone(args.file, args.freq)
    gain, auto = mic_gain(t["description"])
    pa = 10 ** ((args.spl - 94) / 20)
    sens = t["level_dbfs"] - 20 * math.log10(pa)
    print(f"{args.file}: {t['rate']} Hz, mic gain {'unknown' if gain is None else f'{gain} dB'}{' (auto!)' if auto else ''}")
    print(f"  tone at {t['f0']:.1f} Hz: {t['level_dbfs']:.2f} dBFS for {args.spl:.1f} dB SPL")
    print(f"  sensitivity: {sens:.2f} dBFS/Pa at this gain" + (f" ({sens - gain:.2f} dBFS/Pa referred to 0 dB gain)" if gain is not None else ""))
    print(f"  THD {t['thd_pct']:.3f} % ({t['thd_db']:.1f} dB), THD+N {t['thdn_pct']:.3f} % (20 Hz-20 kHz)")
    print("  harmonics: " + ", ".join(f"H{i + 2} {h - t['level_dbfs']:.1f} dB" for i, h in enumerate(t["harmonics_db"][:5])))
    print(f"  peak {t['peak_dbfs']:.1f} dBFS, {t['clipped']} clipped samples; a sine clips at about {args.spl - t['level_dbfs']:.1f} dB SPL at this gain")
    if t["clipped"]:
        print("  CLIPPED: lower the mic gain and record again")


def analyse_noise(path):
    x, rate, description = read_wav(path)
    x = trim(x, rate)
    freqs, power = welch(x, rate, 16384)
    top = min(HIGH_HZ, rate / 2)
    band = (freqs >= LOW_HZ) & (freqs < top)
    flat = float(np.sum(power[band]))
    weighted = float(np.sum(power[band] * 10 ** (a_weighting_db(freqs[band]) / 10)))
    full = float(np.sum(power[1:]))  # everything but DC
    return rate, description, dbfs_from_rms(math.sqrt(flat)), dbfs_from_rms(math.sqrt(weighted)), dbfs_from_rms(math.sqrt(full)), freqs, power


def cmd_noise(args):
    rate, description, flat, weighted, full, freqs, power = analyse_noise(args.file)
    gain, auto = mic_gain(description)
    print(f"{args.file}: {rate} Hz, mic gain {'unknown' if gain is None else f'{gain} dB'}{' (auto!)' if auto else ''}")
    print(f"  noise floor: {flat:.1f} dBFS (20 Hz-20 kHz), {weighted:.1f} dBFS(A), {full:.1f} dBFS (everything to Nyquist)")
    quant = 20 * math.log10(1 / 32768 / math.sqrt(12)) + 3.0  # 16-bit quantisation noise, as dBFS
    print(f"  16-bit quantisation noise would be {quant:.1f} dBFS: the floor is {flat - quant:.1f} dB above it")
    # Where the noise sits: the loudest 1/3-octave bands
    bands = third_octave_bands(3, rate / 2)
    levels = [(c, dbfs_from_rms(math.sqrt(band_power(freqs, power, lo, hi)))) for c, lo, hi in bands]
    loudest = sorted(levels, key=lambda b: -b[1])[:3]
    print("  loudest 1/3-octave bands: " + ", ".join(f"{c:.0f} Hz {l:.1f} dBFS" for c, l in loudest))
    if args.tone:
        t = analyse_tone(args.tone, args.freq)
        tgain, _ = mic_gain(t["description"])
        if gain is not None and tgain is not None and gain != tgain:
            print(f"  NOTE: the tone was recorded at {tgain} dB and the noise at {gain} dB; the figures below assume the same gain")
        ein = args.spl - (t["level_dbfs"] - weighted)
        ein_flat = args.spl - (t["level_dbfs"] - flat)
        clip_spl = args.spl - t["level_dbfs"]
        print(f"  with the tone ({t['level_dbfs']:.2f} dBFS at {args.spl:.1f} dB SPL):")
        print(f"    equivalent input noise {ein:.1f} dB SPL(A) ({ein_flat:.1f} dB SPL unweighted)")
        print(f"    signal-to-noise at {args.spl:.0f} dB SPL: {t['level_dbfs'] - weighted:.1f} dB(A)")
        print(f"    clipping at about {clip_spl:.1f} dB SPL at this gain, so the dynamic range is {clip_spl - ein:.1f} dB(A)")


def spectrum_bands(path, fraction):
    x, rate, description = read_wav(path)
    x = trim(x, rate)
    freqs, power = welch(x, rate, 8192 if rate <= 48000 else 16384)
    bands = third_octave_bands(fraction, rate / 2)
    levels = [dbfs_from_rms(math.sqrt(band_power(freqs, power, lo, hi))) for _, lo, hi in bands]
    return rate, description, [b[0] for b in bands], levels


def write_svg(path, title, series, ymin, ymax, fmax, ylabel):
    """A simple log-frequency plot; series is [(name, centres, values)]."""
    W, H, L, R, T, B = 800, 420, 60, 20, 30, 50
    fx = lambda f: L + (math.log10(f) - math.log10(20)) / (math.log10(fmax) - math.log10(20)) * (W - L - R)
    fy = lambda v: T + (ymax - v) / (ymax - ymin) * (H - T - B)
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" font-family="sans-serif" font-size="12">',
           f'<rect width="{W}" height="{H}" fill="white"/>', f'<text x="{L}" y="18" font-weight="bold">{title}</text>']
    decade = 20
    while decade < fmax:
        for m in (1, 2, 5):
            f = decade * m
            if 20 <= f <= fmax:
                out.append(f'<line x1="{fx(f):.1f}" y1="{T}" x2="{fx(f):.1f}" y2="{H - B}" stroke="#ddd"/>')
                label = f"{f / 1000:g}k" if f >= 1000 else f"{f:g}"
                out.append(f'<text x="{fx(f):.1f}" y="{H - B + 16}" text-anchor="middle">{label}</text>')
        decade *= 10
    step = 10 if ymax - ymin > 40 else 5
    v = math.ceil(ymin / step) * step
    while v <= ymax:
        out.append(f'<line x1="{L}" y1="{fy(v):.1f}" x2="{W - R}" y2="{fy(v):.1f}" stroke="#ddd"/>')
        out.append(f'<text x="{L - 6}" y="{fy(v) + 4:.1f}" text-anchor="end">{v:g}</text>')
        v += step
    out.append(f'<text x="{(L + W - R) / 2:.0f}" y="{H - 8}" text-anchor="middle">Hz</text>')
    out.append(f'<text transform="translate(14 {(T + H - B) / 2:.0f}) rotate(-90)" text-anchor="middle">{ylabel}</text>')
    colours = ["#111", "#c33", "#36c"]
    for i, (name, centres, values) in enumerate(series):
        pts = " ".join(f"{fx(c):.1f},{fy(min(max(v, ymin), ymax)):.1f}" for c, v in zip(centres, values) if c <= fmax)
        out.append(f'<polyline points="{pts}" fill="none" stroke="{colours[i % 3]}" stroke-width="2"/>')
        out.append(f'<text x="{W - R}" y="{T + 16 + 16 * i}" text-anchor="end" fill="{colours[i % 3]}">{name}</text>')
    out.append("</svg>")
    with open(path, "w") as f:
        f.write("\n".join(out))


def cmd_spectrum(args):
    rate, description, centres, levels = spectrum_bands(args.file, args.bands)
    gain, auto = mic_gain(description)
    print(f"{args.file}: {rate} Hz, mic gain {'unknown' if gain is None else f'{gain} dB'}{' (auto!)' if auto else ''}, 1/{args.bands}-octave bands to {rate / 2:.0f} Hz")
    series = [("device", centres, levels)]
    rows = [("Hz", "device dBFS")]
    diff = None
    if args.ref:
        rrate, rdesc, rcentres, rlevels = spectrum_bands(args.ref, args.bands)
        n = min(len(centres), len(rcentres))
        diff = [levels[i] - rlevels[i] for i in range(n)]
        at1k = min(range(n), key=lambda i: abs(centres[i] - 1000))
        diff = [d - diff[at1k] for d in diff]
        series.append(("reference", rcentres[:n], rlevels[:n]))
        rows = [("Hz", "device dBFS", "reference dBFS", "device re reference dB (0 at 1 kHz)")]
        print(f"  reference {args.ref}: {rrate} Hz; the difference is the device's response relative to it")
    for i, c in enumerate(centres):
        row = [f"{c:8.0f}", f"{levels[i]:8.1f}"]
        if diff is not None and i < len(diff):
            row += [f"{series[1][2][i]:8.1f}", f"{diff[i]:+7.1f}"]
        print("  " + "  ".join(row))
    if args.csv:
        with open(args.csv, "w") as f:
            f.write(",".join(rows[0]) + "\n")
            for i, c in enumerate(centres):
                vals = [f"{c:.1f}", f"{levels[i]:.2f}"]
                if diff is not None and i < len(diff):
                    vals += [f"{series[1][2][i]:.2f}", f"{diff[i]:.2f}"]
                f.write(",".join(vals) + "\n")
        print(f"  wrote {args.csv}")
    if args.svg:
        if diff is not None:
            write_svg(args.svg, f"{args.file} relative to {args.ref}", [("device re reference", centres[:len(diff)], diff)], -30, 20, rate / 2, "dB")
        else:
            lo = min(levels)
            write_svg(args.svg, f"{args.file}", series, math.floor(lo / 10) * 10 - 10, 10, rate / 2, "dBFS per band")
        print(f"  wrote {args.svg}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="command", required=True)
    s = sub.add_parser("info", help="what the files say about themselves")
    s.add_argument("files", nargs="+")
    s.set_defaults(func=cmd_info)
    s = sub.add_parser("tone", help="a calibrator tone at a known SPL")
    s.add_argument("file")
    s.add_argument("--spl", type=float, default=94.0, help="the calibrator's level in dB SPL (default 94)")
    s.add_argument("--freq", type=float, default=None, help="the tone's frequency, if it isn't the strongest component")
    s.set_defaults(func=cmd_tone)
    s = sub.add_parser("noise", help="a recording of nothing")
    s.add_argument("file")
    s.add_argument("--tone", help="the calibrator tone recorded at the same gain, for SPL-referred figures")
    s.add_argument("--spl", type=float, default=94.0)
    s.add_argument("--freq", type=float, default=None)
    s.set_defaults(func=cmd_noise)
    s = sub.add_parser("spectrum", help="averaged band levels, optionally relative to a reference microphone")
    s.add_argument("file")
    s.add_argument("--ref", help="a reference microphone's recording of the same source")
    s.add_argument("--bands", type=int, default=3, help="bands per octave (default 3)")
    s.add_argument("--csv")
    s.add_argument("--svg")
    s.set_defaults(func=cmd_spectrum)
    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
