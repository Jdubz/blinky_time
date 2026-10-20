#!/usr/bin/env python3
"""
check_build_libs.py - verify a firmware compile used only the pinned toolchain (#252).

Reads the `arduino-cli compile --verbose` log written by scripts/build.sh and
fails (exit 1) if:
  - any library resolved outside <repo>/.arduino (the isolated toolchain),
  - arduino-cli reported "Multiple libraries were found" (a shadowing choice),
  - a library pinned in tools/firmware_toolchain.json resolved at another version,
  - the core's TinyUSB (Adafruit_TinyUSB_Arduino) was not the one used.
On success it writes the resolved library set as JSON (recorded in release
manifests).

  python tools/check_build_libs.py <compile.log> --out <libraries.json>
"""

import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
USING = re.compile(r"^Using library (.+?) at version (\S+) in folder: (.+?)\s*$")


def norm(p):
    return str(p).replace("\\", "/").rstrip("/").lower()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    text = Path(args.log).read_text(encoding="utf-8", errors="replace")
    root = norm(REPO / ".arduino") + "/"   # separator: .arduino-backup/ is outside
    pinned = {}
    for spec in json.loads((REPO / "tools/firmware_toolchain.json").read_text(encoding="utf-8"))["libraries"]:
        name, ver = spec.rsplit("@", 1)
        pinned[name] = ver

    errors, libs = [], []
    if "Multiple libraries were found" in text:
        block = text[text.index("Multiple libraries were found"):][:600]
        errors.append("arduino-cli chose between multiple libraries:\n" + block)
    for line in text.splitlines():
        m = USING.match(line.strip())
        if not m:
            continue
        name, ver, folder = m.groups()
        libs.append({"name": name, "version": ver, "folder": folder})
        if not norm(folder).startswith(root):
            errors.append(f"{name} {ver} resolved OUTSIDE the pinned toolchain: {folder}")
        if name in pinned and pinned[name] != ver:
            errors.append(f"{name} resolved at {ver}, pinned {pinned[name]}")
    if not libs:
        errors.append("no 'Using library' lines in the log (was --verbose passed?)")
    tusb = [l for l in libs if "TinyUSB" in l["name"]]
    if not tusb or not all("adafruit_tinyusb_arduino" in norm(l["folder"]) for l in tusb):
        errors.append(f"TinyUSB not resolved from the core's Adafruit_TinyUSB_Arduino: {tusb}")

    if errors:
        print("ERROR: build did not use the pinned toolchain (#252):", file=sys.stderr)
        for e in errors:
            print("  - " + e, file=sys.stderr)
        return 1
    out = [{"name": l["name"], "version": l["version"],
            "folder": l["folder"].replace("\\", "/").split("/.arduino/", 1)[-1]} for l in libs]
    Path(args.out).write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
    print(f"  libraries: pinned toolchain only ({len(out)} resolved, recorded in {args.out})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
