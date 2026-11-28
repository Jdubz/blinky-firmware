# Next-gen fixtures: XIAO ESP32-S3 Sense, dual-core

**Status:** phase A done (2026-11-27): the ESP32-S3 build is back, single-core (see "What carries over"). The dual-core split, the flash-write stall work, OTA and the NN investigation are still ahead. Decided 2026-10-15. The current fleet stays on the XIAO nRF52840 (Sense and Sense Plus); nothing here changes deployed devices. The next generation of fixtures is built on the XIAO ESP32-S3 Sense. Owner priority P3 (2026-11-23): ESP32-S3 dual-core, and NN feasibility there.

**The onset NN is shelved on the current fleet.** On the nRF52840 it at best ties SuperFlux on the drum metrics, and a tie goes to flux. Whether a drum-hit NN earns its place on the S3 (its CPU and PSRAM headroom, every-frame inference, beating SuperFlux on kick/snare recall, on-drum share and `repS`) is the P3 investigation. The plan below assumes an NN listener; if the investigation says no, the listener core runs SuperFlux and the architecture is unchanged.

## Why

1. **Visuals come first.** Smooth LED output matters more than audio. On the nRF52840 a single 64 MHz core runs rendering, LED output, BLE and the whole audio pipeline. Audio work (FFT, mel, onset NN, tempo/phase) competes with rendering for the same core. The NN is already skipped on catch-up frames (#165) and stream formatting can stall the loop (#23).
2. **Every unit is fully independent.** Each fixture has its own listener and its own analysis and works alone. Fleet features (broadcast commands, gossip ACK, shared timing) are a bonus layered on top, never a dependency.
3. **The fixtures are being rebuilt anyway.** Any physical change dwarfs firmware cost, so the platform can change now at no extra hardware cost.

## Architecture: one board, two cores

```
Core 0 (PRO_CPU): renderer             Core 1 (APP_CPU): listener
  generators / effects / pipeline        PDM mic (I2S-PDM DMA)
  LED output (RMT or I2S DMA)            FFT -> mel -> onset NN (every frame)
  BLE / fleet / serial console           tempo, phase, PLP pattern
  reads latest AudioControl ------<----- publishes AudioControl per frame
```

- **Renderer has priority by construction.** It owns core 0 and never waits on audio. The LED bit timing is done in hardware (RMT/I2S DMA), so a busy core can't corrupt frames.
- **Listener runs on the audio clock.** It is a task pinned to core 1 and processes every frame, with no catch-up skipping. Its CPU and PSRAM budget allow a larger onset model than the nRF52 can run.
- **Handoff, part 1: a snapshot for the continuous fields.** These are `energy`, `pulse`, `phase`, `plpPulse`, `rhythmStrength` and `onsetDensity`. The listener publishes them each frame through a seqlock or double buffer, with a publish timestamp.
  - The renderer makes a **bounded** number of read attempts, for example 3.
  - If it can't get a consistent copy (the writer is mid-publish or stalled), it reuses its last complete snapshot. It never spins waiting for the writer.
  - Neither side ever blocks the other.
- **Handoff, part 2: events delivered as counters, not as values in the snapshot.** `onset` and `beatCount` are one-shot events. `AudioControl.h` promises exactly one non-zero `onset` per hit, and `Water` edge-triggers on every non-zero read.
  - With a latest-only snapshot on two independent clocks, a slower renderer misses an onset that a newer frame overwrote, and a faster one reads the same onset twice.
  - So the listener keeps monotonic event counters instead: `onsetSeq`, plus the strength of the newest onset, and `beatCount`, which already is one.
  - The renderer remembers the last value it consumed and synthesises one `onset` per draw when the counter has advanced.
  - When several onsets landed between two draws, it emits one onset with the **maximum** strength and records the count. That matches the existing one-event-per-update contract, and the generators' own cooldowns would merge them anyway.
  - Counters are 32-bit and compared with wrap-safe subtraction, so there is no queue and nothing can overflow.
- **Graceful degradation.** The renderer checks the snapshot's age. If no fresh frame has arrived within a timeout (for example 200 ms), or the listener reports no rhythm, it runs organic mode. A frozen or absent audio core must never freeze the visuals.

### The contract stays the same

`AudioControl` (`blinky-things/audio/AudioControl.h`) is the interface between analysis and visuals: `energy`, `level`, `pulse`, `onset`, `phase`, `beatCount`, `plpPulse`, `rhythmStrength`, `onsetDensity`. Generators keep consuming it unchanged. What runs underneath is free to change: the model, the features, even a different listener entirely.

## What carries over

- **Phase A, done (#50):** the current tree builds for the XIAO ESP32-S3 under a pinned toolchain. That toolchain is `tools/firmware_toolchain_esp32s3.json`: arduino-esp32 `3.3.12`, NimBLE-Arduino `2.5.2`, OPI PSRAM, and the `default_8MB` partitions (two OTA app slots, chosen now so OTA won't need a repartition). CI compiles it (`Compile Firmware (ESP32-S3)`), and every staging build and candidate carries `blinky-things-b<N>-esp32s3.bin` (the full flash image at `0x0`) and `.app.bin` (`docs/RELEASE_PROCESS.md`, "The ESP32-S3 image"). It runs single-core: renderer and audio in the normal loop, as on the nRF52840. The nRF52840 build is unchanged (its fingerprint image is byte-identical). What the S3 has in place of the nRF52840's platform code:
  - **LEDs:** `Esp32RmtLedStrip` (RMT; DMA on the one channel that has it). `show()` hands off and never waits, with the same staged-frame / latest-wins contract and `ledFps` / `ledSuperseded` / `ledTxStackFree` as `Nrf52PwmLedStrip`. `ledPin` is a GPIO number (D0 = GPIO1).
  - **Settings and device config:** NVS (`Preferences`, namespace `blinky`), the same `ConfigData` and the same `device upload` JSON. Saves are synchronous NVS writes; whether they hitch a frame is part of the flash-write stall work below.
  - **Serial output:** the `SerialOut` ring and low-priority drain task (#23 / fw#85) on HWCDC. Without it, HWCDC's write waits up to ~2 s for a host that isn't reading.
  - **Watchdog and crash loops:** the task watchdog (15 s, a panic on timeout) with an RTC no-init boot counter. After 3 failed boots the chip parks in ROM download mode. The `RebootFrequencyCounter` lives in NVS (`blinkyboot`) and quarantines the device config after 5 fault resets. `json info` `resetReason` uses the nRF52840's RESETREAS bits.
  - **`json info`:** the same fields as the nRF52840, the stack, heap and `streamDropped` ones included (except `recStackFree`, since record mode is nRF52840-only), with `platform: "esp32s3"`.
  - **BLE:** NUS lines run on the loop task (the queue BleNus got in #173). The advertised name follows the nRF52840's `Blinky-<deviceId>-<sn2>` scheme. The mic (`Esp32PdmMic`, GPIO42/41 on the Sense board) and the fleet advertiser are the March 2026 code, now compiling again.
  - **Removed:** the unsigned network firmware paths (the ArduinoOTA listener with a shared password, and `wifi ota <url>`). Only signed, released firmware reaches a device; signed S3 OTA comes later.
  - **Wi-Fi is off.** The March 2026 Wi-Fi TCP command server (mDNS-advertised, started once `wifi ssid` / `wifi pass` were set) handed the full console to anyone on the network, unauthenticated. It is removed from the build with its credential commands (`WifiManager`, `WifiCommandServer`); `wifi` now answers with an error, and nothing initialises the radio, so BLE is the only radio on. A real, authenticated network design comes later, as its own plan; until then nothing may start a network listener.
  - **Not verified on hardware yet:** the box's S3 has no flash path yet (blinky_time's agent is building it on this image). The first flash will show whether the mic, LEDs and BLE run as built.
- **Portable code:** generators, effects, render pipeline, `AudioTracker`, `SharedSpectralAnalysis` (needs an ESP-DSP FFT in place of CMSIS), `FrameOnsetNN` (TFLite Micro, using ESP-NN kernels), and the settings/config storage logic.
- **Tooling:** the `tests/tracker` harness and corpus gate are platform-independent, and the validation corpus stays `corpus/edm/` (CLAUDE.md).
- **Release discipline:** the same channels apply (stable / candidate / dev, `docs/RELEASE_PROCESS.md`). The S3 image ships in every candidate but isn't gated, signed or promoted yet. The signed-manifest format (`tools/ota_manifest.py`, Ed25519) can be reused for ESP32-S3 OTA.

## Known risks (from the March 2026 cut and since)

| Risk | Notes / mitigation |
|---|---|
| Antenna / RF | Reported in the earlier ESP32-S3 work. Check fixture layout and enclosure early, with BLE range measured inside a built fixture. |
| Sense pin conflicts | On the Sense, the PDM mic shares pins with JTAG/other functions. Fix the pin map before the PCB/fixture layout. |
| Mic calibration | Different mic and gain path. **Mic-level** calibration (hardware gain, levels, normalizer thresholds, noise floor) is redone from scratch for the new mic; don't copy nRF52 values. **End-to-end quality** (the drum metrics and `repS` on `corpus/edm/`, and the tracker metrics) is measured against the same ground truth, so it *is* compared with the nRF52 fleet. That comparison is bench gate 3. |
| Power | Tens of mA active with radio, against a few mA on the nRF52840. Size battery fixtures for it, and measure under real LED and audio load. |
| Flash writes stall both cores | **Unresolved; must be solved before fixtures.** During an ESP-IDF flash erase or write (settings save, OTA), the normal flash path disables the cache and parks the other core. Ordinary tasks on *both* cores are suspended, including IRAM-resident ones, so IRAM placement alone doesn't prevent a render hitch. Options to evaluate on the bench: flash auto-suspend (`CONFIG_SPI_FLASH_AUTO_SUSPEND`) on a flash chip that supports it; LED DMA fed from internal-RAM frame buffers, so output continues while rendering pauses; and scheduling flash writes so they can't overlap visible animation (deferred saves, OTA only in a maintenance state). Any ISR that may run with the cache disabled must keep all its code **and data** in internal RAM. Same class of problem as #191 on the nRF52. |
| Recovery path | The ROM bootloader can't be bricked (BOOT + USB always works), which is better than the nRF52. But there is no equivalent of the nRF52's BLE-DFU path for sealed fixtures: design OTA (dual app partitions with rollback) before sealing anything. |
| Platform split | Two fleets during the transition. Keep `AudioControl` and the fleet protocol identical on both so the server and console treat them the same. |

## Bench gates before building fixtures

1. **Render isolation.** With the listener running at full load, render fps and frame-time jitter on the largest planned LED count match a build with audio disabled.
2. **Every-frame inference** (if the P3 investigation keeps the NN). The onset NN runs on 100 % of frames at 62.5 Hz, with core-1 headroom recorded.
3. **Audio quality.** Kick/snare recall, on-drum share and `repS` on `corpus/edm/` meet or beat the current nRF52 fleet, measured on the new mic.
4. **Robustness.**
   - Settings save and OTA cause no visible render hitch, using whichever flash-concurrency design the bench work picks (see the risks table).
   - A deliberately crashed or stalled listener, including one stalled mid-publish, leaves the visuals running in organic mode.
   - Onsets are neither lost nor doubled when render fps and audio frame rate differ.
5. **Power.** Measured current under typical and worst-case load, against the fixture battery budget.
6. **Recovery drill.** Interrupted OTA → rollback → re-flash, with the fixture sealed.

## Open questions

- LED driver choice: RMT versus I2S-parallel DMA, given the number of strands and LEDs per fixture.
- Whether an NN beats SuperFlux on drum hits at all on the S3 (P3), how large a model the listener can afford, and whether to retrain for the new mic or fine-tune.
- Fleet transport on ESP32-S3: BLE only, or Wi-Fi/ESP-NOW for fleet features (bonus only, never required).
- When to retire the nRF52 fleet, and whether mixed fleets need any protocol changes.

## Multi-platform code

Moved from `docs/DEVELOPMENT.md`. The tree still carries the ESP32-S3 branches from before the March 2026 cut; CI doesn't build them, so expect bit-rot. One codebase is meant to build for the XIAO nRF52840 Sense (and Sense Plus) and the XIAO ESP32-S3 Sense.

### Platform Detection

All platform-specific branching flows through `hal/PlatformDetect.h`:

```cpp
#if defined(ESP32)
  #define BLINKY_PLATFORM_ESP32S3  1
#elif defined(ARDUINO_ARCH_MBED) || defined(ARDUINO_ARCH_NRF52) || \
      defined(NRF52) || defined(NRF52840_XXAA)
  #define BLINKY_PLATFORM_NRF52840 1
#else
  #error "Unsupported platform. Add detection to hal/PlatformDetect.h"
#endif
```

**Rules:**
- Never use raw `#ifdef ESP32` or `#ifdef NRF52` outside `PlatformDetect.h`
- Always use `BLINKY_PLATFORM_*` macros in application code
- Add new platforms to `PlatformDetect.h` first, then implement HAL

### Platform Detection Macros Reference

| Core | Key Macros Defined |
|------|--------------------|
| Seeeduino nRF52 (non-mbed) | `NRF52840_XXAA`, `NRF52` |
| Seeeduino mbed | `ARDUINO_ARCH_MBED`, `ARDUINO_ARCH_NRF52840` |
| arduino-esp32 | `ESP32`, `ARDUINO_ARCH_ESP32` |
| SAMD (Arduino Zero etc.) | `ARDUINO_ARCH_SAMD` |
| AVR | `__AVR__`, `ARDUINO_ARCH_AVR` |

Note: `ARDUINO_ARCH_*` macros come from `platform.txt` as `-DARDUINO_ARCH_{build.arch}`. They are only present when that core is installed and selected.

### Adding Platform-Specific Code

**Preferred pattern — HAL interface + per-platform implementation:**

```
hal/
├── interfaces/IPdmMic.h        ← abstract interface (no platform code)
├── hardware/Nrf52PdmMic.h/cpp  ← nRF52840 implementation
├── hardware/Esp32PdmMic.h/cpp  ← ESP32-S3 implementation
└── DefaultHal.h                ← factory: returns correct impl at compile time
```

The `DefaultHal.h` factory pattern:
```cpp
#ifdef BLINKY_PLATFORM_NRF52840
  inline Nrf52PdmMic& pdm() { static Nrf52PdmMic i; return i; }
#elif defined(BLINKY_PLATFORM_ESP32S3)
  inline Esp32PdmMic& pdm() { static Esp32PdmMic i; return i; }
#endif
```

**Guard .cpp files at the file level:**
```cpp
// Nrf52PdmMic.cpp
#include "hal/PlatformDetect.h"
#ifdef BLINKY_PLATFORM_NRF52840
#include <PDM.h>
// ... all nRF52-specific implementation ...
#endif
```

This prevents the ESP32 toolchain from seeing `PDM.h` (which doesn't exist on ESP32).

### Key Platform Differences

| Feature | nRF52840 | ESP32-S3 |
|---------|----------|----------|
| PDM mic | Hardware PDM interrupt → `onReceive()` callback | I2S peripheral, polling via `poll()` in `AdaptiveMic::update()` |
| Flash storage | settings journal + `InternalFileSystem` | `Preferences` (NVS key-value store) |
| Battery monitor | GPIO + ADC divider circuit | Not available — all pins set to -1 |
| Compile output | `.hex` → `.uf2` via `uf2conv.py` | `.ino.merged.bin` (full flash image, `0x0`) and `.ino.bin` (app, `0x10000`); flashed with esptool, no UF2 |
| LED pin numbers | Arduino pin numbers (D0..D10) | GPIO numbers (D0 = GPIO1) |

### Common Pitfalls

**Library not available on target platform**
Symptom: `fatal error: PDM.h: No such file or directory` when compiling for ESP32.
Fix: Guard the `#include` and the `.cpp` file with `#ifdef BLINKY_PLATFORM_NRF52840`.

**Library .cpp files cannot be gated from the sketch**
Arduino compiles library `.cpp` files independently — sketch-level `#define` values do not propagate. Always put platform guards inside the library `.cpp` file itself (or in its header).

**Struct layout differs across platforms**
The nRF52 (ARM Cortex-M4) and ESP32-S3 (Xtensa LX7) compilers may pad structs differently. `ConfigStorage` stores structs to flash — if struct layout diverges between platforms, the stored data becomes unreadable. Mitigations:
- Use `__attribute__((packed))` on stored structs
- Use fixed-width types (`uint32_t`, not `int`)
- Validate struct size with `static_assert` (will fail at compile time if padding changed)
- Each platform has a separate NVS namespace / flash region

**`ARDUINO_ARCH_NRF52` vs `NRF52840_XXAA`**
The mbed and non-mbed Seeeduino nRF52 cores define different macros. `PlatformDetect.h` handles both. Do not add new detection logic outside that file.

### Compiling for Both Platforms

```bash
./scripts/build.sh --no-bump          # nRF52840, inside the pinned toolchain
./scripts/build.sh --esp32 --no-bump  # ESP32-S3, inside its pinned toolchain (.arduino-esp32s3/)
```

Or via Makefile: `make compile-out` and `make esp32-compile`. Don't call `arduino-cli compile` directly: it would bypass the pinned toolchains.
