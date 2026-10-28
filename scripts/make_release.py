"""Prepares the web flasher: copies the built binaries into docs/firmware/ and
writes docs/manifest.json for ESP Web Tools.

    python3 scripts/make_release.py [version]

Builds the `release` environment (the firmware without src/wifi_config.h, so
no credentials end up in the public binary) and checks the binary for the
network name from that file. The version defaults to the date and the short
git revision. GitHub Pages serves docs/, so after committing and pushing, the
flasher at https://<user>.github.io/<repo>/ installs this build.
"""

import datetime
import glob
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, ".pio", "build", "release")
DOCS = os.path.join(ROOT, "docs")
FIRMWARE = os.path.join(DOCS, "firmware")

# Flash layout of the ESP32-S3 Arduino core
PARTS = [
    ("bootloader.bin", 0x0),
    ("partitions.bin", 0x8000),
    ("boot_app0.bin", 0xE000),
    ("zvukopis.bin", 0x10000),
]


def find_boot_app0():
    for path in glob.glob(os.path.expanduser("~/.platformio/packages/framework-arduinoespressif32*/tools/partitions/boot_app0.bin")):
        return path
    sys.exit("boot_app0.bin not found in the PlatformIO packages")


def main():
    if len(sys.argv) > 1:
        version = sys.argv[1]
    else:
        rev = subprocess.run(["git", "rev-parse", "--short", "HEAD"], capture_output=True, text=True, cwd=ROOT).stdout.strip()
        version = datetime.date.today().isoformat() + (f"-{rev}" if rev else "")
    if subprocess.run(["pio", "run", "-e", "release"], cwd=ROOT).returncode != 0:
        sys.exit("The release build failed")
    firmware = open(os.path.join(BUILD, "firmware.bin"), "rb").read()
    config = os.path.join(ROOT, "src", "wifi_config.h")
    if os.path.exists(config):
        # The credentials defined there must not be in the binary
        for name, value in re.findall(r'#define\s+(WIFI_SSID|WIFI_PASSWORD|HOTSPOT_PASSWORD)\s+"([^"]+)"', open(config).read()):
            if value.encode() in firmware:
                sys.exit(f"The release binary contains {name} from src/wifi_config.h; not publishing it")

    os.makedirs(FIRMWARE, exist_ok=True)
    sources = {
        "bootloader.bin": os.path.join(BUILD, "bootloader.bin"),
        "partitions.bin": os.path.join(BUILD, "partitions.bin"),
        "boot_app0.bin": find_boot_app0(),
        "zvukopis.bin": os.path.join(BUILD, "firmware.bin"),
    }
    for name, source in sources.items():
        shutil.copyfile(source, os.path.join(FIRMWARE, name))
        print(f"{name:16s} {os.path.getsize(source):>9} bytes")

    manifest = {
        "name": "zvukopis",
        "version": version,
        "new_install_prompt_erase": True,  # a fresh board may hold factory data; the dialog offers to wipe it
        "builds": [{
            "chipFamily": "ESP32-S3",
            "parts": [{"path": f"firmware/{name}", "offset": offset} for name, offset in PARTS],
        }],
    }
    with open(os.path.join(DOCS, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print(f"Wrote docs/manifest.json, version {version}")


if __name__ == "__main__":
    main()
