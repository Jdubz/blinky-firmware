# Development Guide - blinky-firmware

## First-time setup

After cloning (`git clone --recurse-submodules`, or `git submodule update --init` in an existing clone), install the pinned firmware toolchain once (`bash scripts/setup_toolchain.sh`; see [BUILD_GUIDE.md](BUILD_GUIDE.md)). Then install the git pre-push hook, which runs cppcheck with CI's settings when cppcheck is installed:

```bash
./scripts/install-hooks.sh
```

The hook source is in `hooks/pre-push`; the script copies it into `.git/hooks/`. Bypass for a single push with `git push --no-verify` (use sparingly — every CI failure that the hook would have caught is a wasted round-trip).

The fleet server, console and ML training are [blinky_time](https://github.com/Jdubz/blinky_time)'s; their development guide is that repo's `docs/DEVELOPMENT.md`.

## 🚨 Critical Safety Rules

### Upload Method (nRF52840)

**NEVER use `arduino-cli upload`, `make upload`, or `adafruit-nrfutil dfu serial` on the nRF52840.** The DFU serial protocol can brick the device.

**Only stable releases go to fleet and new devices.** Use:
1. **Fleet devices:** blinky_time's `./scripts/deploy.sh --devices=<id|all|list>`.
2. **A device on this machine's USB:** `python tools/deploy_local.py --port COMx --device-id <id>` (`--dry-run` first). `--dev` builds and release candidates are refused unless the device is listed in `devices/test_devices.json`.
3. **A bricked bare chip that isn't in the fleet yet:** `make uf2-upload UPLOAD_PORT=/dev/ttyACM0`.

Channels and the release flow: [RELEASE_PROCESS.md](RELEASE_PROCESS.md). Don't drag a local `.uf2` onto a configured device; it skips the identity and version checks.

> ESP32-S3 is next-gen only ([NEXTGEN_ESP32S3_PLAN.md](NEXTGEN_ESP32S3_PLAN.md)); it isn't in the fleet.

### Recovery (nRF52840 bricked by DFU upload)

Test chips on the test box recover over SWD through their XIAO Debug Mate: `scripts/swd_recover.sh` (see [SWD_RECOVERY.md](SWD_RECOVERY.md)). A Raspberry Pi Zero W at `swd-flash.local` is provisioned for SWD recovery (see CLAUDE.md for invocation). It has the known-good fleet bootloader, MBR, and SoftDevice S140 7.3.0 hex files staged. Use this as the recovery of last resort when a device drops off USB and isn't reachable via BLE.

### Unresponsive Device Runbook

Installed nRF52840 devices are physically enclosed — reset buttons are **not** accessible. If a device stops responding to serial commands:

1. **Power-cycle the USB port via hub control** *(bus-powered devices only)*:
   ```bash
   uhubctl -a cycle -p <port>
   ```
   Does NOT work on `cart_inner` / `cart_outer` — those are externally powered, so toggling the host USB hub only affects data lines. For those, use the firmware `bootloader` serial command, SWD via `swd-flash.local`, or the physical reset button.

2. **Re-flash via blinky_time's deploy.sh** (forces a known-good firmware state):
   ```bash
   ./scripts/deploy.sh --devices=<device_id>
   ```
   Direct `curl` against `/api/devices/{id}/flash` is forbidden — the server enforces an `X-Deploy-Tool` header that only `deploy.sh` sets (returns 403 otherwise). See CLAUDE.md's upload-safety section.

3. **If the serial port disappeared entirely:** wait 10 seconds and check `ls /dev/ttyACM*`. USB re-enumeration after DFU or crash recovery is not instantaneous.

4. **Last resort:** SWD via `swd-flash.local`, or physical access if available. Most production fleet is sealed — exhaust the SWD path first.

> **Test chips (unenclosed bare chips on bench):** always test risky or untested firmware on these first. Reset button and SWD recovery pads are directly accessible.

> **Post-BLE-DFU quirk:** After a BLE DFU flash, USB serial does not re-enumerate without a physical power cycle. BLE reconnection works fine. See blinky_time's `docs/BLUETOOTH_IMPLEMENTATION_PLAN.md`.

---

## 📋 Pre-Deploy Workflow

For any firmware deployment to fleet devices, the canonical workflow is blinky_time's:

```bash
./scripts/deploy.sh --devices=<id|all|list>   # latest stable release → upload → flash → verify
```

`deploy.sh` never compiles. It downloads the latest **stable** GitHub release (or `--release bN`, stable only), checks its sha256, flashes through blinky-server and verifies the version. A change reaches the fleet only after it is merged, built as a candidate, bench-gated and promoted, when the owner decides to release ([RELEASE_PROCESS.md](RELEASE_PROCESS.md)). To try your own build, use `./scripts/build.sh --dev` and `tools/deploy_local.py --dev` on a test device.

`scripts/pre_compile_safety.py` runs only as part of the Makefile `compile-out` target (`make safety-check`); `build.sh` doesn't call it.

For bare-chip recovery (device not enrolled in fleet), `make uf2-upload UPLOAD_PORT=/dev/ttyACM0`.

---

## Platform code

All platform branching goes through `hal/PlatformDetect.h` and its `BLINKY_PLATFORM_*` macros; never use raw `#ifdef ESP32` / `#ifdef NRF52` elsewhere. Platform-specific code sits behind a HAL interface (`hal/interfaces/`, `hal/hardware/`, `hal/DefaultHal.h`), and platform `.cpp` files are guarded at file level. The ESP32-S3 side (macros, HAL pattern, platform differences, pitfalls, building) is in [NEXTGEN_ESP32S3_PLAN.md](NEXTGEN_ESP32S3_PLAN.md#multi-platform-code).

---

## ⚙️ Config Version Management

### When to Increment SETTINGS_VERSION

**ALWAYS increment** when you:
- Add new fields to `StoredFireParams` or `StoredMicParams`
- Remove fields from config structs
- Change field types (e.g., `float` → `double`)
- Reorder struct fields

### How to Increment SETTINGS_VERSION

**File: `blinky-things/config/ConfigStorage.h`**. Bump the current value by one; never reuse a number an older layout had. The example below uses 100 → 101 (100 was current when this was written; check the file).

```cpp
// Before:
static const uint8_t SETTINGS_VERSION = 100;

// After:
static const uint8_t SETTINGS_VERSION = 101;  // v101: describe your changes here
```

Note: a `SETTINGS_VERSION` bump keeps the device config (LED layout, device name, etc.) but resets every runtime setting to its default on each device (boot log: "Settings version mismatch, using defaults (device config preserved)"). Release notes flag it for that reason.

### Required Steps After Struct Changes

1. **Increment SETTINGS_VERSION** in ConfigStorage.h
2. **Update comment** with description of changes
3. **Add validation** for new parameters in ConfigStorage.cpp `loadConfiguration()`
4. **Update static_assert** for struct size in ConfigStorage.h
5. **Update defaults** in `loadSettingsDefaults()` (the settings-version mismatch path calls only that, not `loadDefaults()`)
6. **Update load/save** in `loadConfiguration()` and `saveConfiguration()`
7. **Run safety script**: `python scripts/check_config_safety.py`

### Example: Adding a New Parameter

```cpp
// 1. Add to struct (ConfigStorage.h)
struct StoredMicParams {
    // ... existing fields ...
    float newParameter;  // NEW: describe purpose
};

// 2. Increment version (current + 1)
static const uint8_t SETTINGS_VERSION = 101;  // v101: added newParameter for XYZ

// 3. Update static_assert (StoredMicParams is currently 8 bytes; sizes for
//    other structs are in ConfigStorage.h ~lines 394-406)
static_assert(sizeof(StoredMicParams) == 12,  // Updated from 8
    "StoredMicParams size changed! Increment SETTINGS_VERSION and update assertion.");

// 4. Add default (ConfigStorage.cpp loadSettingsDefaults())
data_.mic.newParameter = 1.0f;  // Reasonable default

// 5. Add validation (ConfigStorage.cpp loadConfiguration())
validateFloat(data_.mic.newParameter, 0.0f, 10.0f, F("newParameter"));

// 6. Add to load (ConfigStorage.cpp loadConfiguration())
mic.newParameter = data_.mic.newParameter;

// 7. Add to save (ConfigStorage.cpp saveConfiguration())
data_.mic.newParameter = mic.newParameter;
```

---

## 🛡️ Safety Mechanisms

### Compile-Time Checks

**Static Assertions** (ConfigStorage.h) — one per stored struct, e.g.:
```cpp
static_assert(sizeof(StoredMicParams) == 8,
    "StoredMicParams size changed! Increment SETTINGS_VERSION and update assertion.");
```

**Purpose**: Catches struct size changes at compile time

**If this fails**: You forgot to increment SETTINGS_VERSION or update the assertion

### Runtime Checks

**Struct Size Logging** (debug output at boot):
```
[DEBUG] ConfigData: ...B (MicParams: ...
```

**Purpose**: Helps debug padding issues across platforms

**Parameter Validation** (ConfigStorage.cpp):
```cpp
validateFloat(data_.mic.peakTau, 0.5f, 10.0f, F("peakTau"));
```

**Purpose**: Catches corrupt/garbage data from flash. An out-of-range value is logged at `[ERROR]`, counted in `BlinkyAssert::failCount` (visible in `show errors`), and clamped so the device still boots. Integers use the `VALIDATE_INT` macro.

### Safety Tests

**Flash Address Validation** (SafetyTest.h):
- Prevents writing to bootloader region
- Validates all flash operations before execution

**Heap/Stack Tests**:
- Validates memory allocation works correctly
- Detects corruption early

---

## 🧪 Testing Best Practices

### Test Before Upload

`scripts/pre_compile_safety.py` runs automatically as part of `make compile-out` (not `build.sh` or `deploy.sh`). The `SafetyTest::runAllTests()` runtime harness in `blinky-things/tests/SafetyTest.h` is no longer wired into the standard build — invoke it from a dedicated test sketch if needed.

### Use Serial Console for Tuning

Prefer runtime tuning over reflashing:
```
list                # List settings
get <name>          # Check current value
set <name> <value>  # Modify value
save                # Persist to flash
```

`save` (and the auto-save 5 s after the last `set`) returns at once. A writer task does the flash write in the background (blinky_time#191). `reboot`, `bootloader` and `ota commit` wait for a pending write before resetting, so nothing saved is lost.

**Known defect:** a settings page erase can still pause a frame for up to ~90 ms. That breaks the rule that LEDs never drop frames, and it is to be fixed, not accepted.

---

## 🔍 Common Pitfalls

### ❌ Forgot to Increment SETTINGS_VERSION

**Symptom**: The `static_assert` fails at compile time (size changed), or, if the size didn't change, saved values load into the wrong fields

**Cause**: Old flash data read with new struct layout

**Fix**: Increment SETTINGS_VERSION, update the assertion, add validation

### ❌ Missing Parameter Validation

**Symptom**: Random crashes or strange behavior

**Cause**: Garbage data from flash read into unvalidated parameter

**Fix**: Add a `validateFloat()` or `VALIDATE_INT()` call

### ❌ Struct Padding Changed

**Symptom**: Config loads incorrectly across platforms

**Cause**: Compiler padding differs (32-bit vs 64-bit, different compilers)

**Fix**: Order fields by size (floats, uint16_t, uint8_t), use `#pragma pack` if needed

### ❌ Used `arduino-cli upload` on nRF52840

**Symptom**: Device bricked, reset button does nothing

**Cause**: DFU serial protocol corrupted bootloader

**Fix**: SWD recovery via `swd-flash.local` (see Recovery above). From then on, flash with `deploy.sh` / `deploy_local.py`.

---

## 📝 Code Review Checklist

Before merging config changes:

- [ ] SETTINGS_VERSION incremented if struct changed
- [ ] Version comment describes changes
- [ ] Static assertions updated
- [ ] New parameters have validation
- [ ] Defaults set in `loadSettingsDefaults()`
- [ ] Load/save methods updated
- [ ] Safety script passes: `python scripts/check_config_safety.py`
- [ ] Compiled cleanly: `./scripts/build.sh --no-bump` (ESP32 path optional — `esp32-compile` only if touching `BLINKY_PLATFORM_ESP32S3`-guarded code)
- [ ] No `arduino-cli upload` usage on nRF52840 in commit history

Before merging platform-specific code:

- [ ] Platform detection goes through `hal/PlatformDetect.h` — no raw `#ifdef ESP32` in app code
- [ ] Platform `.cpp` files have file-level `#ifdef BLINKY_PLATFORM_*` guards
- [ ] New HAL interfaces have a default no-op or stub for platforms that don't need them
- [ ] Struct sizes verified consistent across platforms (or documented as platform-local)

---

## 🔗 Additional Resources

- [SafetyTest Documentation](../blinky-things/tests/SafetyTest.h)
- [SerialConsole Commands](../blinky-things/inputs/SerialConsole.cpp)
- [Architecture Overview](../blinky-things/README.md)
- [XIAO nRF52840 Recovery Guide](https://www.pavelp.cz/posts/eng-xiao-nrf52840-bootloader-recovery/)

---

## 💡 Pro Tips

1. **Use git branches** for risky changes
2. **Test config changes** with multiple power cycles
3. **Keep a backup device** for development
4. **Document WHY**, not just what, in comments
5. **Serial logging** is your friend - add generous debug output
6. **Reset tunables** with `restore_runtime_settings` on the serial console if settings get corrupted. `wipe_device_identity` also wipes the device config; on fleet devices it is deploy.sh-gated.
