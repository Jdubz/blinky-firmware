#!/usr/bin/env bash
# Install the pinned firmware toolchain into <repo>/.arduino (#252).
#
#   bash scripts/setup_toolchain.sh                    # nRF52840 (the fleet) -> .arduino
#   bash scripts/setup_toolchain.sh --target esp32s3   # XIAO ESP32-S3 (next gen) -> .arduino-esp32s3
#
# Every machine that builds firmware (workstations, the fleet server, CI, the
# release workflow) runs this, so every build resolves exactly the same core
# and libraries: tools/firmware_toolchain.json, TFLite pinned in
# tools/setup_tflite_lib.sh, the core patches in patches/. scripts/build.sh
# and the Makefile build only inside this environment.
#
# The ESP32-S3 toolchain (tools/firmware_toolchain_esp32s3.json) is the same
# kind of pin in its own directory: core and libraries only (no TFLite, no
# core patches; both are nRF52840-specific today).
#
# Idempotent. Fails loudly if anything is missing, extra, or unpatched.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
case "${1:-}" in
    "") export BLINKY_TARGET=nrf52840 ;;
    --target)
        [[ -n "${2:-}" ]] || { echo "ERROR: --target needs nrf52840 or esp32s3" >&2; exit 1; }
        export BLINKY_TARGET="$2" ;;
    *) echo "ERROR: unknown argument '$1' (usage: setup_toolchain.sh [--target nrf52840|esp32s3])" >&2; exit 1 ;;
esac
source scripts/arduino_env.sh
NRF=false
[[ "$BLINKY_TARGET" == nrf52840 ]] && NRF=true

if ! command -v arduino-cli >/dev/null 2>&1; then
    echo "ERROR: arduino-cli not found on PATH" >&2
    exit 1
fi
blinky_require_cli_version

mkdir -p "$BLINKY_ARDUINO_DIR/data" "$BLINKY_ARDUINO_DIR/user/libraries" "$BLINKY_ARDUINO_DIR/staging"
# Directories and URLs come from the environment (arduino_env.sh); the config
# file only exists so no user-level config file is read instead.
printf 'directories: {}\n' > "$BLINKY_ARDUINO_DIR/arduino-cli.yaml"

echo "=== Pinned toolchain -> $BLINKY_ARDUINO_DIR ==="
arduino-cli core update-index
arduino-cli core install "$BLINKY_CORE"

# Plain read loop (macOS ships bash 3.2, which has no mapfile).
"$BLINKY_PYTHON" -c "import json,sys; print('\n'.join(json.load(open(sys.argv[1]))['libraries']))" \
    "$(_blinky_native "$BLINKY_TOOLCHAIN_JSON")" | tr -d '\r' > "$BLINKY_ARDUINO_DIR/.libs"
while IFS= read -r lib; do
    [[ -n "$lib" ]] && arduino-cli lib install "$lib"
done < "$BLINKY_ARDUINO_DIR/.libs"
rm -f "$BLINKY_ARDUINO_DIR/.libs"

if $NRF; then
    bash tools/setup_tflite_lib.sh --ci
    bash scripts/apply_core_patches.sh
fi

# Verify: exactly the pinned libraries (plus TFLite on the nRF52840), nothing else.
"$BLINKY_PYTHON" - "$(_blinky_native "$BLINKY_TOOLCHAIN_JSON")" "$NRF" <<'EOF'
import json, subprocess, sys
tflite = sys.argv[2] == "true"
want = {}
for spec in json.load(open(sys.argv[1]))["libraries"]:
    name, ver = spec.rsplit("@", 1)
    want[name] = ver
out = subprocess.run(["arduino-cli", "lib", "list", "--format", "json"],
                     capture_output=True, text=True, encoding="utf-8", errors="replace",
                     check=True).stdout
data = json.loads(out or "{}")
rows = data.get("installed_libraries", data) if isinstance(data, dict) else data
have = {}
for r in rows or []:
    lib = r.get("library", r)
    have[lib["name"]] = lib.get("version", "")
bad = []
for name, ver in want.items():
    if have.get(name) != ver:
        bad.append(f"{name}: want {ver}, have {have.get(name)}")
extra = sorted(set(have) - set(want) - ({"Arduino_TensorFlowLite"} if tflite else set()))
if extra:
    bad.append(f"unexpected libraries: {extra}")
if tflite and "Arduino_TensorFlowLite" not in have:
    bad.append("Arduino_TensorFlowLite missing")
if bad:
    sys.exit("ERROR: toolchain library set wrong:\n  " + "\n  ".join(bad))
print("  libraries:", ", ".join(f"{k}@{v}" for k, v in sorted(have.items())))
EOF
if $NRF; then
    bash scripts/apply_core_patches.sh --check
fi
echo "=== Toolchain ready ($BLINKY_CORE) ==="
