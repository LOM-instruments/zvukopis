#!/usr/bin/env python
"""Checks the USB microphone mode from a Mac.

Run with PlatformIO's python (it has pyserial), with Zvukopis already in
Settings > USB microphone (scripts/serial_session.py can put it there:
send:m, eight send:d, send:c). It needs ffmpeg and sox (brew install ffmpeg sox).

    python scripts/usbmic_test.py --out DIR [--seconds 10] [--gain-steps 2] [--tone] [--leave]

It waits for the "Zvukopis microphone" audio device and the serial port next
to it, keeps the port's log in DIR/mic_log.txt, records the microphone with
ffmpeg, then looks at the recording: the rate, the level, and, with --tone,
whether the tone's phase ever jumps (a lost or empty USB packet shows as a
jump). --tone plays a quiet 1 kHz tone on the laptop speaker during the
recording; say so before running it, nobody likes a surprise beep. --gain-steps presses the rocker
up that many times over the serial port halfway through, and --leave sends
the restart at the end.
"""

import argparse
import glob
import math
import os
import struct
import subprocess
import sys
import threading
import time
import wave

import serial

DEVICE = "Zvukopis microphone"


def audio_device_present():
    out = subprocess.run(["system_profiler", "SPAudioDataType"], capture_output=True, text=True).stdout
    return DEVICE in out


def find_port(exclude):
    for p in sorted(glob.glob("/dev/cu.usbmodem*")):
        if p not in exclude:
            return p
    return None


def wait_for(what, check, timeout):
    start = time.time()
    while time.time() - start < timeout:
        r = check()
        if r:
            print(f"{what}: {r if r is not True else 'yes'} after {time.time() - start:.1f} s")
            return r
        time.sleep(0.5)
    print(f"{what}: not within {timeout} s")
    return None


class Log(threading.Thread):
    def __init__(self, port, path):
        super().__init__(daemon=True)
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = 115200
        self.ser.dtr = True  # the device's serial port only talks once DTR and RTS are up
        self.ser.rts = True
        self.ser.timeout = 0.2
        self.ser.open()
        self.file = open(path, "w")
        self.stop = False

    def run(self):
        while not self.stop:
            data = self.ser.read(4096)
            if data:
                text = data.decode("utf-8", "replace")
                self.file.write(text)
                self.file.flush()
                for line in text.splitlines():
                    print("   | " + line)

    def send(self, chars):
        self.ser.write(chars.encode())
        self.ser.flush()


def analyse(path, tone=True):
    with wave.open(path, "rb") as w:
        rate, channels, width, frames = w.getframerate(), w.getnchannels(), w.getsampwidth(), w.getnframes()
        raw = w.readframes(frames)
    print(f"recording: {rate} Hz, {channels} ch, {width * 8}-bit, {frames} frames = {frames / rate:.2f} s")
    samples = struct.unpack(f"<{frames * channels}h", raw)[::channels]
    peak = max(abs(s) for s in samples)
    rms = math.sqrt(sum(s * s for s in samples) / len(samples))
    print(f"level: peak {20 * math.log10(max(peak, 1) / 32768):.1f} dBFS, rms {20 * math.log10(max(rms, 1) / 32768):.1f} dBFS")
    zeros = sum(1 for s in samples if s == 0)
    print(f"exact zeros: {zeros} ({100 * zeros / len(samples):.2f} %)")
    # The tone: zero-crossing intervals should be steady (rate / 1000 / 2 samples
    # for 1 kHz). A lost packet shortens one interval by a packet's worth.
    if not tone:
        return
    crossings = [i for i in range(1, len(samples)) if (samples[i - 1] < 0) != (samples[i] < 0)]
    if len(crossings) > 100:
        intervals = [b - a for a, b in zip(crossings, crossings[1:])]
        expect = rate / 1000 / 2
        odd = [(crossings[i], d) for i, d in enumerate(intervals) if abs(d - expect) > expect * 0.5]
        print(f"tone: {len(crossings)} zero crossings, interval {sum(intervals) / len(intervals):.1f} samples (1 kHz would be {expect:.0f}); {len(odd)} odd intervals")
        for at, d in odd[:12]:
            print(f"   at {at / rate:.3f} s: {d} samples")
    else:
        print("tone: not found in the recording (is the speaker on, is the mic near it?)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--seconds", type=int, default=10)
    ap.add_argument("--gain-steps", type=int, default=0)
    ap.add_argument("--tone", action="store_true", help="play a quiet 1 kHz tone on the speaker while recording")
    ap.add_argument("--leave", action="store_true")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    if not wait_for("audio device", audio_device_present, 20):
        sys.exit(1)
    port = wait_for("serial port", lambda: find_port(set()), 10)  # in this mode the only one is the microphone's
    log = Log(port, os.path.join(args.out, "mic_log.txt")) if port else None
    if log:
        log.start()
    time.sleep(2)

    wav = os.path.join(args.out, "mic.wav")
    # ffmpeg, not sox: sox's coreaudio driver cannot open the device at 96 kHz
    rec = subprocess.Popen(["ffmpeg", "-y", "-loglevel", "error", "-f", "avfoundation", "-i", ":" + DEVICE,
                            "-t", str(args.seconds), "-ac", "1", "-ar", "96000", "-c:a", "pcm_s16le", wav])
    tone = subprocess.Popen(["play", "-q", "-n", "synth", str(args.seconds), "sine", "1000", "vol", "0.1"]) if args.tone else None
    if args.gain_steps and log:
        time.sleep(args.seconds / 2)
        for _ in range(args.gain_steps):
            log.send("u")
            time.sleep(1.5)
    rec.wait()
    if tone:
        tone.wait()
    time.sleep(6)  # one more log line
    if os.path.exists(wav):
        analyse(wav, args.tone)
    else:
        print("no recording was made")
    if log and args.leave:
        print("leaving the mode (the device restarts)")
        log.send("q")
        time.sleep(1)
    if log:
        log.stop = True


if __name__ == "__main__":
    main()
