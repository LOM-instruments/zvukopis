# Stamps the firmware with its version: today's date and the short commit
# hash, the same string scripts/make_release.py writes into docs/manifest.json
# (plus a "+" when the working tree had uncommitted changes, or a file in
# src/ or lib/ that git does not know: that is compiled in too). Shown in
# Settings on the device and on the web interface.
import datetime
import subprocess

Import("env")

root = env["PROJECT_DIR"]


def git(*args):
    try:
        return subprocess.run(["git", *args], capture_output=True, text=True, cwd=root).stdout.strip()
    except OSError:
        return ""


rev = git("rev-parse", "--short", "HEAD")
dirty = "+" if git("status", "--porcelain", "--untracked-files=no") or git("ls-files", "--others", "--exclude-standard", "--", "src", "lib") else ""
version = datetime.date.today().isoformat() + (f"-{rev}{dirty}" if rev else "")
env.Append(CPPDEFINES=[("FIRMWARE_VERSION", env.StringifyMacro(version))])
print(f"Firmware version {version}")
