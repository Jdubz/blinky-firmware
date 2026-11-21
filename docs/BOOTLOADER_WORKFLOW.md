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

## 6. Open work

- [ ] Fork CI merged; release variant byte-identical to the committed fleet UF2 (provenance).
- [ ] First tagged release `bl-0.8.0-10` from that build; blinky-firmware switches to the release pin and stops committing the UF2.
- [ ] Gate runner in blinky_time's test-box code (`scripts/testbox/`), built on `swd_recover.sh`, `deploy-bootloader.sh`, `uhubctl` and the BLE DFU tool; posts status to the fork.
- [ ] Enumeration-loop watchdog that powers off the port.
- [ ] Repeat the no-app DFU test (gate step 5) on a working XIAO. 06ACEB's 3V3 rail failed (2026-11-21), so the earlier bench evidence can't be trusted.
- [ ] Debug Mate firmware with a USB serial number (source build), only if a second probe is ever added.
