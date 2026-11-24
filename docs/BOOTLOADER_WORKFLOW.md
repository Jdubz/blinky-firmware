# Bootloader Workflow

How the blinky bootloader is developed, built, released, tested and recovered.
Owner decisions (2026-11-21):
- The bootloader is **our fork**, maintained separately. Fix bugs there; don't file upstream or wait on Adafruit.
- The test box has **one** XIAO nRF52840 Sense Plus (87DE9B58C90AFE2D), seated in a XIAO Debug Mate. The Debug Mate is for debugging and recovery only.
- Every normal process works over **USB alone, and preferably also BLE**.
- **Everything is hands-off.** No step needs a person, and every validation runs on that one chip.

## 1. Where things live

| What | Where |
|---|---|
| Source | `Jdubz/Adafruit_nRF52_Bootloader`, branch `blinky-local-patches` (protected; changes by PR). `master` is only a reference copy of upstream; cherry-pick from it when useful. |
| CI | the fork's `.github/workflows/blinky-bootloader.yml`. Every push/PR builds release and debug variants with arm-none-eabi-gcc 9-2019-q4, runs `tools/verify_bootloader.py`, and uploads the ELF, map, hexes, UF2s and SHA256SUMS. |
| Releases | a `bl-<version>` tag on the fork publishes a GitHub Release (release variant, plus a debug tarball). |
| Consumer | blinky-firmware pins the source as the `bootloader/src` submodule (at the tagged commit) and the binaries by release tag + sha256. No hand-built binaries are committed. |

## 2. Change flow

1. **Branch and PR in the fork.** CI must be green, including the verifier, and the review clean.
2. **Merge to `blinky-local-patches`.** CI builds it, and that build is the candidate.
3. **Hardware gate on the test box** (section 3), run against the CI artifact. The result posts as a commit status on the fork.
4. **Tag `bl-0.8.0-N`** after the gate passes; CI publishes the Release.
5. **blinky-firmware PR:** bump the submodule to the tag and the release pin (tag + sha256). A CI check that the submodule commit equals the tagged commit keeps source and binary tied together.
6. **Fleet rollout** only through `scripts/deploy-bootloader.sh`, over USB (UF2) or BLE DFU, never SWD.

## 3. The hardware gate (one chip, hands-off)

It runs on the test box's single XIAO. Normal steps use USB (and BLE where noted). SWD is used **only** to set up a known state and to recover.

1. **Baseline:** `swd_recover.sh restore` with the *current* fleet bootloader and the latest stable app, then confirm the app enumerates over USB.
2. **Install the candidate the way the fleet will get it:** `deploy-bootloader.sh` over USB (UF2), then check `INFO_UF2.TXT` reports the candidate's version.
3. **App boots** after the update (USB enumerates as the app; CDC `json info` answers).
4. **DFU entry from the app over USB** (CDC `bootloader`): the UF2 drive appears and **stays up for at least 60 s**.
5. **No-app DFU** (the 06ACEB failure mode): invalidate the app with the bootloader's own UF2 path or by a CDC command, *not* SWD. The chip must enumerate as the UF2 drive and **stay enumerated for at least 60 s**. BLE DFU advertising must be visible at the same time.
6. **UF2 app install** from DFU, and the app boots.
7. **BLE DFU** (preferred path): push the stable app over BLE (`tools/push_ble_dfu.py`), and the app boots.
8. **App-handshake watchdog:** three failed app boots force DFU, then a good app clears it.
9. **Downgrade back to the fleet bootloader** over USB, so the chip ends in the fleet state.

Respect the 60-second rule between resets (CLAUDE.md). Any step failing, or the chip stopping enumerating, triggers **recovery** (section 4) and fails the gate. The gate always ends with the chip back on the fleet bootloader and the stable app.

## 4. Automatic recovery

- **Detection:** the box watches for a USB enumeration loop (the kernel reporting `-71`/`Cannot enable` on the chip's port) or no enumeration within a timeout. On a loop, it **switches the chip's port off at once**: a looping device makes the hub reset its own upstream link.
- **Recovery:** `scripts/swd_recover.sh restore` (CTRL-AP erase → golden image with the fleet bootloader + stable app + valid settings page → verify), then a cold boot.
- **Cold boot:** the chip must have **one** 5V source (section 5). Power-cycle that hub port with `uhubctl`. The Belkin F4U058 cuts VBUS on each port independently (verified 2026-11-21).
- **Retries are capped.** After two failed recoveries the box marks the chip down and alerts; it doesn't loop.

## 5. Bench wiring rules

- **One 5V source.** The XIAO's 5V pin is on the same net as its USB VBUS, so the Debug Mate's 5V otherwise flows back through the XIAO's cable into the hub port. Either:
  - power from the Debug Mate and connect the XIAO's USB **with VBUS blocked** (data-only cable or adapter); the nRF still sees VBUS through the 5V pin; **or**
  - power from the XIAO's USB with the Debug Mate's 5V contact disabled.
- **Never hot-swap** the XIAO in the Debug Mate while powered (Seeed guidance).
- **Run one OpenOCD at a time** (`swd_recover.sh` enforces it). Never write POWER/CLOCK registers from the debugger while the SoftDevice may be running.

## 5a. Boards: Sense and Sense Plus

New devices and the test chip are XIAO nRF52840 **Sense Plus**; the legacy fleet is the plain **Sense**. **Every bootloader and firmware build must run on both.** From Seeed's netlists (2026-11-21):
- **Every pin we use is identical:** QSPI flash P0.20–P0.25, PDM mic (P1.00/P0.16, power P1.10), IMU, LEDs P0.26/P0.30/P0.06, reset/DFU P0.18, P0.03, both crystals, and D0–D10. The bootloader's `xiao_nrf52840_ble_sense` definition fits both, and so does the app built for `xiaonRF52840Sense`.
- **The Plus adds 9 castellated edge pads (D11–D19).** D16 is P0.31 (battery sense) and D14/D15 are the NFC pins. The battery divider is 499k on the Plus vs 510k on the Sense (about 1.4%).
- **Never drive P0.17** (charger status). The Plus schematic labels its charger BQ25100, probably wrongly. **Never enable REG0 DC/DC** (`DCDCEN0`); REG1 DC/DC (`DCDCEN`) is fine.
- **Identity:**
  - A fresh Plus enumerates as `2886:8065` with its factory app and `2886:0065` in its factory bootloader. Once our bootloader and app are installed, it reports `0x8045`/`0x0045`, like a Sense.
  - Legacy Sense units on Seeed's 0.6.1 bootloader report Board-ID `Seeed_XIAO_nRF52840_Sense`, not `nRF52840-SeeedXiaoSense-v1`.
  - The tools accept both PID sets (blinky-firmware #40).
  - **No hardware strap distinguishes the boards.** If the variant matters, record it while the factory bootloader is still there (e.g. a UICR CUSTOMER word).
- **Debug Mate seating:** the J21 contact block lands only on the SWDIO/SWCLK/RST/GND pads of both boards, and on bare solder mask for the ESP32-only BOOT/MTCK contacts. The extra J5/J2 rows can't reach a seated XIAO. Both boards are safe to seat (from Seeed's footprints). **Leave the 5V and 3V3 header pins unsoldered.**
- **The gate runs on the Plus test chip.** A release also needs one check on a legacy Sense (identity matching, and the 0.6.1-bootloader Board-ID path) until the fleet is uniform.

## 5b. Sealed devices: recovery layers, install checks, residual risk

An installed sculpture has no reachable reset button, USB port or SWD pads, so a brick after install is a lost sculpture. The recovery layers, all landed (code comments cite them as F1–F6):
- **F1** The bootloader is built with `DEFAULT_TO_OTA_DFU`: an invalid app, or any DFU entry that doesn't ask for UF2, ends in DFU with BLE (and USB, per the dual-transport invariant) advertising until a host pushes firmware.
- **F2** The old USB-only `SafeMode` trap was deleted. No code path can leave the device in a loop without BLE.
- **F3** `SafeBootWatchdog::markStable()` runs only after 60 s of uptime, so a crash after `setup()` still counts toward the 3-strikes BLE-DFU recovery. This is why CLAUDE.md's 60-second rule exists.
- **F4** `hal/RebootFrequencyCounter` is flash-backed, so power cycles don't reset it. It counts fault resets only (watchdog or lockup, since b215); at the threshold it quarantines the device config and enters BLE DFU (see the header comment for its fail-safe modes).
- **F5** BLE-DFU entry sets `GPREGRET = 0xA8` as well as the RAM magic (`SafeBootWatchdog.h`).
- **F6** A failed `bleDfu.begin()` logs `[FALLBACK]` instead of failing silently.
- On top of these, the 0.8.0-10 app-handshake watchdog forces DFU after 3 app boots without the firmware's `GPREGRET` clear, which closes the crashy-but-valid-app gap.

**Install checks.** Run on every unit before it is sealed:

| # | Check | Covers |
|---|---|---|
| T1 | Erase the app, power-cycle: the device advertises BLE DFU on its own and the fleet server reflashes it | F1 |
| T2 | An image that crashes in `setup()`: 3 strikes trigger BLE DFU and the fleet recovers it | F3 |
| T3 | An image that runs past the 60 s `markStable()` window, then faults with a watchdog or lockup reset before the counter's 5-minute clear: after 5 such boots the flash counter enters BLE DFU, and the stored device config is quarantined (`quarantineDeviceConfig()`). A fault before 60 s is F3's case, so it doesn't exercise this. | F4 |
| T4 | T3 with a power cycle between every crash: it still triggers. Only fault resets count, so the power cycles themselves don't. | F4 |
| T5 | Start a BLE DFU upload, drop the link partway, power-cycle: the device comes back in BLE DFU | F1 |
| T7 | Signed QSPI OTA: (a) a chunk with a bad CRC is rejected while staging; (b) a staged image whose SHA-512 doesn't match the signed manifest is refused by `ota commit`. In both cases the old app keeps running. | QSPI staging |
| T8 | A full fleet flash over BLE DFU completes and the device reconnects | BLE DFU |

**Residual risk** (no software mitigation; low probability, one sculpture each):
- **Power loss during a bootloader self-update.** Don't update the bootloader on a sealed unit until #10 gives a safe BLE path.
- **SoftDevice or bootloader flash corruption.** Validate SoftDevice and bootloader integrity before install.
- **A BLE stack crash inside the bootloader's DFU session** (nRF52 errata). A pre-install soak catches most.
- **Hardware failure** (flash wear, antenna, regulator).
- **The DFU transport's inactivity timer** returns a valid-but-buggy app to its crash loop after a failed session. F4 re-enters DFU on a later boot, so the server gets repeated windows rather than one continuous one.
- **The bootloader's legacy BLE DFU is unsigned** (SDK 11). The app's own update path is signed (`docs/WEB_FLASHER.md`).

## 6. Open work

- [x] Fork CI merged (Jdubz/Adafruit_nRF52_Bootloader#2, 2026-11-21). Builds are reproducible: two runs of one commit give identical ELF/hex/UF2/zip. Provenance: the CI build of 0.8.0-10 matches the committed fleet UF2 except for the version string and `__DATE__`/`__TIME__` bytes.
- [ ] First tagged release (`bl-*`, cut with the next bootloader change: tags must contain the workflow, so 0.8.0-10 itself can't be re-released under its own version string); blinky-firmware switches to the release pin and stops committing the UF2.
- [ ] Gate runner in blinky_time's test-box code (`scripts/testbox/`), built on `swd_recover.sh`, `deploy-bootloader.sh`, `uhubctl` and the BLE DFU tool; posts status to the fork.
- [ ] Enumeration-loop watchdog that powers off the port.
- [x] No-app DFU test (gate step 5) on a healthy board (04F2A796, 2026-11-21): PASS. It stays enumerated in DFU for 150 s after a cold boot. BLE advertising wasn't checked: the box runs with Bluetooth off at the OS level by design (`dtoverlay=disable-bt`, until per-server device ownership exists), so the BLE half of the gate needs a host where BLE is allowed.
- [x] **blinky-server recovers a chip in its USB bootloader, hands-off** (blinky_time#378, #379; live 2026-11-21 on 04F2A796). It discovers `2886:0045`/`0065` as a DFU-recovery device and waits a 120 s grace. It then flashes the armed stable image through the single flash path, onto the drive bound by USB serial (sudo-mount fallback under systemd), and verifies. Recovery is armed by a stable deploy through `POST /api/fleet/deploy` (or `deploy.sh` on the fleet host).
- [ ] SWD escalation in blinky-server: when USB recovery gives up, or the chip never enumerates, run `swd_recover.sh restore` and cold-boot both ports.
- [x] SWD restore tier (`swd_recover.sh restore`) verified hands-off on 04F2A796: backup, CTRL-AP erase, golden image + verify, reset in 3.5 min; the chip came back on b217.
- [ ] Debug Mate firmware with a USB serial number (source build), only if a second probe is ever added.
- [ ] **Bootloader self-update over BLE** (#10), so a sealed device's bootloader is never stuck.
- [ ] **BLE DFU negotiates MTU 23** (#11): 20-byte chunks, about 6.5 min per device.
- [ ] **0.8.0-11, parked** (owner decision; draft fork PR Jdubz/Adafruit_nRF52_Bootloader#1, not flashed). It arms the stuck-transfer check only after a valid UF2 block, which fixes UF2 recovery from Windows. Rollout requirements are in `docs/WEB_FLASHER.md` (Recovery, and rollout checklist item 4).
- [ ] Optional, no deadline:
  - A UF2 first-write hook that sets `dfu_startup_packet_received`, so the 3 s startup timeout is cancelled by the MSC path too, not only by `tud_mounted()`.
  - Replace `is_ota()` with `sd_softdevice_is_enabled()` wherever it really means "is the SoftDevice running?" (in dual-transport mode it is up regardless of the DFU trigger).
  - Tune `MSC_STUCK_TIMEOUT_MS` (8000 in `msc_uf2.c`) if a host is found that legitimately pauses more than 8 s between WRITE10s in one transfer. 0.8.0-11 changes what re-arms this timer.
