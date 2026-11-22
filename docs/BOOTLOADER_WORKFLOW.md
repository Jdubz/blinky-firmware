# Bootloader Workflow

How the blinky bootloader is developed, built, released, tested and recovered.
Owner decisions (2026-11-21):
- The bootloader is **our fork**, maintained separately. Fix bugs there; don't file upstream or wait on Adafruit.
- The test box has **one** XIAO nRF52840 Sense, seated in a XIAO Debug Mate. The Debug Mate is for debugging and recovery only.
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

## 6. Open work

- [x] Fork CI merged (Jdubz/Adafruit_nRF52_Bootloader#2, 2026-11-21). Builds are reproducible: two runs of one commit give identical ELF/hex/UF2/zip. Provenance: the CI build of 0.8.0-10 matches the committed fleet UF2 except for the version string and `__DATE__`/`__TIME__` bytes.
- [ ] First tagged release (`bl-*`, cut with the next bootloader change: tags must contain the workflow, so 0.8.0-10 itself can't be re-released under its own version string); blinky-firmware switches to the release pin and stops committing the UF2.
- [ ] Gate runner in blinky_time's test-box code (`scripts/testbox/`), built on `swd_recover.sh`, `deploy-bootloader.sh`, `uhubctl` and the BLE DFU tool; posts status to the fork.
- [ ] Enumeration-loop watchdog that powers off the port.
- [x] No-app DFU test (gate step 5) on a healthy board (04F2A796, 2026-11-21): PASS. It stays enumerated in DFU for 150 s after a cold boot. BLE advertising wasn't checked, because the Pi's Bluetooth scan returned nothing.
- [ ] **blinky-server can't see a USB device that's in the bootloader.** Serial discovery accepts only `2886:8045`, and bootloader-mode recovery exists only over BLE (`AdaDFU`), which the box runs without (`--no-ble`). Test 2026-11-21: a chip with no app sat in DFU for 10 min while the server logged `0 serial` and never recovered it. Fix: discover `2886:0045`/`0065` (bootloader CDC + UF2 drive) as a DFU-recovery device and route it through the single flash path (`FleetManager.flash_device`) with the latest stable release. Also add the Sense Plus app PID `0x8065`.
- [x] SWD restore tier (`swd_recover.sh restore`) verified hands-off on 04F2A796: backup, CTRL-AP erase, golden image + verify, reset in 3.5 min; the chip came back on b217.
- [ ] Debug Mate firmware with a USB serial number (source build), only if a second probe is ever added.
