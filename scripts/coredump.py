#!/usr/bin/env python
"""Reads the core dump a crash left in flash and prints every task's backtrace.

    python scripts/coredump.py [--port /dev/cu.usbmodem1101] [--out DIR]

Run with PlatformIO's python (it has pyserial). The firmware writes a core
dump to the coredump partition (0xFF0000 in default_16MB.csv) on a panic, an
abort() or a task watchdog; this pulls it out with esptool (the board is
then reset into the app), strips the 20-byte flash header and the checksum,
and hands the ELF core to the toolchain's gdb with the firmware just built.
The boot banner's reset reasons say whether there was a crash at all
(4 = panic, 6 = task watchdog).
"""

import argparse
import glob
import os
import struct
import subprocess
import sys

HOME = os.path.expanduser("~")
ESPTOOL = os.path.join(HOME, ".platformio/packages/tool-esptoolpy/esptool.py")
GDB = os.path.join(HOME, ".platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-gdb")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(ROOT, ".pio/build/esp32s3-epaper/firmware.elf")
PARTITION = 0xFF0000
SIZE = 0x10000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None)
    ap.add_argument("--out", default=os.path.join(ROOT, ".pio", "coredump"))
    ap.add_argument("--elf", default=ELF, help="the firmware the device was running")
    args = ap.parse_args()
    port = args.port or (glob.glob("/dev/cu.usbmodem*") or [None])[0]
    if not port:
        sys.exit("no serial port; is the board on USB and awake?")
    os.makedirs(args.out, exist_ok=True)
    raw = os.path.join(args.out, "core.bin")
    subprocess.run([sys.executable, ESPTOOL, "--chip", "esp32s3", "--port", port, "--baud", "921600", "--after", "no_reset",
                    "read_flash", hex(PARTITION), hex(SIZE), raw], check=True, stdout=subprocess.DEVNULL)
    subprocess.run([sys.executable, ESPTOOL, "--chip", "esp32s3", "--port", port, "--after", "watchdog_reset", "chip_id"],
                   check=True, stdout=subprocess.DEVNULL)
    data = open(raw, "rb").read()
    total, version = struct.unpack("<II", data[:8])
    if total == 0xFFFFFFFF or total > SIZE:
        sys.exit("no core dump in flash")
    elf = data[0x14 : total - 4]
    if elf[:4] != b"\x7fELF":
        sys.exit("the dump is not in the ELF format this script expects")
    core = os.path.join(args.out, "core.elf")
    open(core, "wb").write(elf)
    for line in data.split(b"\0"):
        if b"abort()" in line or b"Guru" in line or b"watchdog" in line:
            print(line.decode("utf-8", "replace").strip())
    print(f"core dump: {len(elf)} bytes, version {version:#x}; tasks:")
    subprocess.run([GDB, "-q", "-batch", "-ex", "set pagination off", "-ex", "info threads", "-ex", "thread apply all bt 12",
                    args.elf, "-c", core])


if __name__ == "__main__":
    main()
