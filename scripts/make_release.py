"""Prepares the web flasher: copies the built binaries into docs/firmware/ and
writes docs/manifest.json for ESP Web Tools.

    python3 scripts/make_release.py [version]
    python3 scripts/make_release.py --publish

Builds the `release` environment (the firmware without src/wifi_config.h, so
no credentials end up in the public binary) and checks the binary for the
network name from that file. The version defaults to the date and the short
git revision. GitHub Pages serves docs/, so after committing and pushing, the
flasher at https://<user>.github.io/<repo>/ installs this build.

--publish does the rest as well: commits docs/ as "Release <version>", pushes,
and creates a GitHub Release tagged with the version, the four binaries and
the manifest attached, and notes made from the commits since the previous
release. Run on a commit that is already a release (the tree clean, HEAD a
"Release ..." commit), it only publishes that one, so a failed publish can
be retried.
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


def git(*args, check=True):
    result = subprocess.run(["git", *args], capture_output=True, text=True, cwd=ROOT)
    if check and result.returncode != 0:
        sys.exit(f"git {' '.join(args)} failed:\n{result.stderr.strip()}")
    return result.stdout.strip()


def previous_release():
    """The tag of the latest GitHub Release, or the latest "Release ..." commit
    before HEAD, or nothing at all (the first release)."""
    out = subprocess.run(["gh", "release", "list", "--limit", "1", "--json", "tagName", "--jq", ".[0].tagName"],
                         capture_output=True, text=True, cwd=ROOT).stdout.strip()
    if out:
        git("fetch", "-q", "origin", "--tags", check=False) # the tag lives on GitHub; the log needs it here
        if git("rev-parse", "-q", "--verify", out + "^{commit}", check=False):
            return out
    commits = git("log", "--format=%H %s", "-n", "400").splitlines()
    for line in commits[1:]:  # not HEAD itself, which may be the release being published
        sha, _, subject = line.partition(" ")
        if subject.startswith("Release "):
            return sha
    return None


def release_notes(version, code_commit):
    """Commit subjects (and bodies) since the previous release, as Markdown."""
    since = previous_release()
    span = f"{since}..{code_commit}" if since else code_commit
    log = git("log", span, "--format=%x1e%s%x1f%b", check=False)
    items = []
    for entry in log.split("\x1e"):
        if not entry.strip():
            continue
        subject, _, body = entry.partition("\x1f")
        if subject.startswith("Release "):
            continue
        paragraph = []
        for line in body.splitlines():
            if line.startswith("Co-Authored-By:") or not line.strip():
                if paragraph:
                    break
                continue
            paragraph.append(line.strip())
        item = f"- **{subject.strip()}**"
        if paragraph:
            item += "  \n  " + " ".join(paragraph)
        items.append(item)
    notes = [f"Firmware `{version}`, built from commit `{code_commit[:7]}`.", ""]
    notes += items if items else ["- The same firmware as the previous release, republished."]
    notes += ["", "**Install:** the [web flasher](https://zvukopis.lom.audio/) (Chrome or Edge, over USB), "
              "or with esptool and the attached files:", "",
              "```", "esptool.py --chip esp32s3 --port /dev/cu.usbmodem1101 --baud 921600 --after no_reset write_flash "
              "0x0 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 zvukopis.bin", "```", "",
              "Settings and the moments on the card are kept. Settings on the device shows the version it runs."]
    return "\n".join(notes) + "\n"


def publish(version):
    """Commits docs/, pushes, tags and creates the GitHub Release."""
    head_subject = git("log", "-1", "--format=%s")
    if head_subject.startswith("Release ") and version in head_subject:
        print(f"HEAD is already the release commit for {version}; publishing it")
        code_commit = git("rev-parse", "HEAD^")
    else:
        code_commit = git("rev-parse", "HEAD")
        git("add", "docs")
        git("commit", "-q", "-m", f"Release {version}", "-m", f"Firmware built from commit {code_commit[:7]}.")
        print(f"Committed docs/ as Release {version}")
    git("push", "-q", "origin", "HEAD")
    print("Pushed")
    notes = release_notes(version, code_commit)
    notes_path = os.path.join(ROOT, ".pio", f"release-notes-{version}.md")
    with open(notes_path, "w") as f:
        f.write(notes)
    assets = [os.path.join(FIRMWARE, name) for name, _ in PARTS] + [os.path.join(DOCS, "manifest.json")]
    cmd = ["gh", "release", "create", version, "--title", f"zvukopis {version}", "--notes-file", notes_path,
           "--target", git("rev-parse", "HEAD"), *assets]
    result = subprocess.run(cmd, cwd=ROOT)
    if result.returncode != 0:
        sys.exit("gh release create failed; fix and run again with --publish on this commit")
    print(f"Published https://github.com/LOM-instruments/zvukopis/releases/tag/{version}")


def main():
    publishing = "--publish" in sys.argv
    args = [a for a in sys.argv[1:] if a != "--publish"]
    if publishing:
        # Already a release commit: just publish it (a retry, or the first release of an existing build)
        head_subject = git("log", "-1", "--format=%s")
        manifest_version = json.load(open(os.path.join(DOCS, "manifest.json"))).get("version", "") if os.path.exists(os.path.join(DOCS, "manifest.json")) else ""
        if head_subject.startswith("Release ") and manifest_version and manifest_version in head_subject:
            if git("status", "--porcelain", "--untracked-files=no"):
                sys.exit("Commit first")
            publish(manifest_version)
            return
    if args:
        version = args[0]
    else:
        dirty = subprocess.run(["git", "status", "--porcelain", "--untracked-files=no"], capture_output=True, text=True, cwd=ROOT).stdout.strip()
        if dirty:
            sys.exit("Commit first: the firmware's version stamp must match a commit\n" + dirty)
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
    if publishing:
        publish(version)


if __name__ == "__main__":
    main()
