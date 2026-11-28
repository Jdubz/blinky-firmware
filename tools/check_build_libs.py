#!/usr/bin/env python3
"""
check_build_libs.py - verify a firmware compile used only the pinned toolchain (#252).

Reads the `arduino-cli compile --verbose` log written by scripts/build.sh and
fails (exit 1) if:
  - any library resolved outside the target's isolated toolchain
    (<repo>/.arduino for the nRF52840, <repo>/.arduino-esp32s3 for the ESP32-S3),
  - arduino-cli reported "Multiple libraries were found" (a shadowing choice),
  - a library pinned in the target's toolchain file (tools/firmware_toolchain.json,
    tools/firmware_toolchain_esp32s3.json) resolved at another version,
  - nRF52840 only: the core's TinyUSB (Adafruit_TinyUSB_Arduino) was not the one used.
On success it writes the resolved library set as JSON (recorded in release
manifests).

  python tools/check_build_libs.py <compile.log> [--target nrf52840|esp32s3] --out <libraries.json>
"""

import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
# target -> (isolated toolchain directory, pin file); scripts/arduino_env.sh has the same table.
TARGETS = {
    "nrf52840": (".arduino", "tools/firmware_toolchain.json"),
    "esp32s3": (".arduino-esp32s3", "tools/firmware_toolchain_esp32s3.json"),
}
USING = re.compile(r"^Using library (.+?) at version (\S+) in folder: (.+?)\s*$")


def norm(p):
    return str(p).replace("\\", "/").rstrip("/").lower()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--out", required=True)
    ap.add_argument("--target", choices=sorted(TARGETS), default="nrf52840")
    args = ap.parse_args()
    tc_dir, tc_file = TARGETS[args.target]

    text = Path(args.log).read_text(encoding="utf-8", errors="replace")
    root = norm(REPO / tc_dir) + "/"   # separator: .arduino-backup/ is outside
    pinned = {}
    for spec in json.loads((REPO / tc_file).read_text(encoding="utf-8"))["libraries"]:
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
    for name in pinned:
        if args.target != "nrf52840" and not any(l["name"] == name for l in libs):
            # Every ESP32-S3 pin is a library the sketch uses; one missing from the
            # log means the pin is stale (or the library was renamed upstream).
            errors.append(f"pinned library {name} was not used by the compile")
    if args.target == "nrf52840":
        tusb = [l for l in libs if "TinyUSB" in l["name"]]
        if not tusb or not all("adafruit_tinyusb_arduino" in norm(l["folder"]) for l in tusb):
            errors.append(f"TinyUSB not resolved from the core's Adafruit_TinyUSB_Arduino: {tusb}")

    if errors:
        print("ERROR: build did not use the pinned toolchain (#252):", file=sys.stderr)
        for e in errors:
            print("  - " + e, file=sys.stderr)
        return 1
    out = [{"name": l["name"], "version": l["version"],
            "folder": l["folder"].replace("\\", "/").split(f"/{tc_dir}/", 1)[-1]} for l in libs]
    Path(args.out).write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
    print(f"  libraries: pinned toolchain only ({len(out)} resolved, recorded in {args.out})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
