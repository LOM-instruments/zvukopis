"""Drives zvukopis over its USB serial port for testing.

    python3 scripts/serial_session.py /dev/cu.usbmodem101 wait:6 send:r wait:3 send:r wait:4 send:s wait:4

Items are "wait:SECONDS" or "send:CHARS" (each character is one serial
command, see main.cpp). Output lines are printed. Screenshot dumps
(SCREENSHOT-BEGIN/END, from the 's' command) are decoded to shot_N.png and
audio dumps (AUDIO-BEGIN/END, 'a') to audio_N.raw in --out (default: the
current directory). Needs pyserial; the PlatformIO Python has it:
/opt/homebrew/Cellar/platformio/*/libexec/bin/python.

The port is opened with DTR and RTS low, which doesn't reset the board.
"""

import base64
import os
import re
import struct
import sys
import time
import zlib

import serial

W, H = 800, 480  # the panel's buffer, landscape


def write_png(path, data, rotation):
    """Writes the 1-bit buffer upright (undoing the canvas rotation)."""
    bit = lambda px, py: (data[py * W // 8 + px // 8] >> (7 - px % 8)) & 1
    if rotation in (1, 3):
        w, h = H, W
        phys = (lambda lx, ly: (W - 1 - ly, lx)) if rotation == 1 else (lambda lx, ly: (ly, H - 1 - lx))
        rows = bytearray()
        for ly in range(h):
            rows.append(0)
            for bx in range(w // 8):
                byte = 0
                for k in range(8):
                    byte = (byte << 1) | bit(*phys(bx * 8 + k, ly))
                rows.append(byte)
    else:
        w, h = W, H
        rows = b"".join(b"\x00" + data[y * W // 8:(y + 1) * W // 8] for y in range(H))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 1, 0, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(bytes(rows))) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def main():
    args = sys.argv[1:]
    out = "."
    if "--out" in args:
        i = args.index("--out")
        out = args[i + 1]
        del args[i:i + 2]
    port, script = args[0], args[1:]
    os.makedirs(out, exist_ok=True)

    ser = None
    buf = ""
    shot, nshot, rotation = None, 0, 0
    audio, naudio = None, 0
    t0 = time.time()
    b64 = re.compile(r"^[A-Za-z0-9+/=]{1,80}$")

    def open_port():
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = port, 115200, 0.1
        s.dtr = s.rts = False
        s.open()
        return s

    def handle(line):
        nonlocal shot, nshot, rotation, audio, naudio
        if line.startswith("SCREENSHOT-BEGIN"):
            shot = []
            rotation = int(line.split("rotation=")[1]) if "rotation=" in line else 0
        elif line == "SCREENSHOT-END" and shot is not None:
            nshot += 1
            path = os.path.join(out, f"shot_{nshot}.png")
            write_png(path, base64.b64decode("".join(shot)), rotation)
            shot = None
            print(f"[{time.time() - t0:.1f}s saved {path}]")
        elif shot is not None and b64.match(line):
            shot.append(line)
        elif line.startswith("AUDIO-BEGIN"):
            audio = []
            print(line)
        elif line == "AUDIO-END" and audio is not None:
            naudio += 1
            path = os.path.join(out, f"audio_{naudio}.raw")
            with open(path, "wb") as f:
                f.write(base64.b64decode("".join(audio)))
            audio = None
            print(f"[{time.time() - t0:.1f}s saved {path}]")
        elif audio is not None and b64.match(line):
            audio.append(line)
        else:
            print(line)

    def pump(until):
        nonlocal ser, buf
        while time.time() < until:
            if ser is None:
                try:
                    ser = open_port()
                except Exception:
                    time.sleep(0.05)
                    continue
            try:
                data = ser.read(4096)
            except Exception:
                ser = None
                continue
            if not data:
                continue
            buf += data.decode(errors="replace")
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                handle(line.rstrip("\r"))

    for item in script:
        kind, _, value = item.partition(":")
        if kind == "wait":
            pump(time.time() + float(value))
        elif kind == "send":
            pump(time.time() + 0.05)
            if ser:
                print(f"[{time.time() - t0:.1f}s >>> {value}]")
                ser.write(value.encode())
    if ser:
        ser.close()


if __name__ == "__main__":
    main()
