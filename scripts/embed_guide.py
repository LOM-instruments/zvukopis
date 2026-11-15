# Builds the user guide into the firmware: docs/guide.md, as it is when the
# firmware is built, becomes GUIDE_TEXT in a header in the build folder, which
# src/library.cpp includes. On the device the library keeps that text in /doc
# on the card, so the guide there is always the one of the firmware it runs.
import os

Import("env")

source = os.path.join(env["PROJECT_DIR"], "docs", "guide.md")
folder = os.path.join(env.subst("$BUILD_DIR"), "generated")
header = os.path.join(folder, "guide_text.h")
LIMIT = 512 * 1024  # MAX_DOCUMENT_BYTES in src/library.h: the library lists nothing larger

with open(source, "rb") as f:
    guide = f.read()
if not guide.strip():
    env.Exit(f"{source} is empty: the firmware needs the user guide")
if len(guide) > LIMIT:
    env.Exit(f"{source} is {len(guide)} bytes: the device's library takes documents up to {LIMIT}")

rows = [", ".join(str(b) for b in guide[i:i + 24]) for i in range(0, len(guide), 24)]
text = (
    "// docs/guide.md as it was when this firmware was built. Made by\n"
    "// scripts/embed_guide.py at every build: change the guide, not this file.\n"
    "#pragma once\n\n#include <stddef.h>\n#include <stdint.h>\n\n"
    f"static const size_t GUIDE_LENGTH = {len(guide)};\n"
    "static const uint8_t GUIDE_TEXT[] = {\n    " + ",\n    ".join(rows) + "};\n"
)
os.makedirs(folder, exist_ok=True)
try:
    with open(header, encoding="utf-8") as f:
        unchanged = f.read() == text
except OSError:
    unchanged = False
if not unchanged:  # written only when the guide changed, so nothing is compiled again for nothing
    with open(header, "w", encoding="utf-8") as f:
        f.write(text)
env.Append(CPPPATH=[folder])

# The device's fonts hold ASCII. Its reader gives typographic quotes, dashes
# and a few accented letters their plain forms (toAscii in src/library.cpp);
# anything else shows as a question mark, so say which signs those are.
PLAIN = set("‘’‚′ʼ“”„″–—―…   «»éèêáàâäöóôüúíîçß")
odd = sorted({c for c in guide.decode("utf-8", errors="replace") if ord(c) > 126 and c not in PLAIN})
print(f"User guide: {len(guide)} bytes of docs/guide.md go into the firmware" + ("" if unchanged else " (changed)"))
if odd:
    print("  The device shows these signs of the guide as '?': " + " ".join(odd))
