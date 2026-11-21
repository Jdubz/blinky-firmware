# SWD Recovery: XIAO Debug Mate + nRF52840 test chips

How the test box recovers a bricked XIAO nRF52840 Sense over SWD. Covers how
the hardware behaves, the recovery procedure (`scripts/swd_recover.sh`), the
traps we hit, and the 2026-11-21 recovery of 06ACEB. Researched and
bench-verified 2026-11-21; sources are at the end.

**Short version:** `./scripts/swd_recover.sh probe`, then
`./scripts/swd_recover.sh restore`. That CTRL-AP-erases the chip and programs
MBR + S140 7.3.0 + the fleet bootloader + UICR + the latest stable app + a
"valid app" settings page, verifying as it goes. It works even when the chip
is stuck in a reset loop. Test chips only: it erases everything.

---

## 1. The probe: Seeed XIAO Debug Mate

| Fact | Consequence |
|---|---|
| ESP32-S3R8 running Seeed_Arduino_DAPLink: a port of ARM's CMSIS-DAP reference, **v1 HID only** (64-byte reports, one packet buffer, 2 ms poll). It reports "FW 2.0.0" anyway. SWD is bit-banged with `digitalWrite`. | Slow (~8 KB/s programming). `adapter speed` probably has no real effect. |
| Enumerates as `0d28:0204` "Espressif Systems CMSIS-DAP" (HID + a CDC UART bridge). At power-up it shows `303a:1001` and then `2886:0056` "XIAO_ESP32S3" for ~5 s first. | Wait for `0d28:0204` before using it. |
| **The target RST pad is wired to ESP32 GPIO47, but the firmware drives nRESET on GPIO45, which is unconnected.** OpenOCD's pin readout (`nRESET=0`, `SWCLK=0`) is meaningless: those reads are hard-coded or come from floating pins. | **There is no hardware reset.** Every reset is SYSRESETREQ (OpenOCD's nrf52.cfg default) or CTRL-AP. "Connect under reset" modes in OpenOCD, pyOCD and probe-rs can't work. Never configure `reset_config srst_*`. |
| **The USB serial number is empty** (Seeed fixed this on firmware `main` in July 2026, after the v1.2 release). | **pyOCD can't open the probe.** Its open matches on the real serial and fails ("Probe … not found") even though `pyocd list` shows it. OpenOCD works. Flash newer probe firmware before trying pyOCD again. Two probes on one host can't be told apart until then. |
| The target is powered from the Debug Mate's 5V pin through an ideal diode. Target GND returns through the power-analyser shunt (10 Ω at the lowest range). | The chip runs even with its own USB cable unplugged: **VBUSDETECT=1 doesn't prove the chip's USB cable is connected.** |
| The target sits on pogo pins under the XIAO socket. Seeed's advice for older boards such as the nRF52840: shift the board slightly and seat it straight. | Re-seat first if SWD is flaky with a halted (stable) core. |
| Debugging only works while the Debug Mate shows its main dashboard page. | Leave the display on the dashboard. |

**Host setup (one time):** `sudo apt install openocd`, plus a udev rule so the
HID node isn't root-only. `/etc/udev/rules.d/60-xiao-debug-mate.rules`:

```
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="0d28", ATTRS{idProduct}=="0204", MODE="0660", GROUP="plugdev", TAG+="uaccess"
SUBSYSTEM=="usb", ATTRS{idVendor}=="0d28", ATTRS{idProduct}=="0204", MODE="0660", GROUP="plugdev", TAG+="uaccess"
```

Then `sudo udevadm control --reload && sudo udevadm trigger --subsystem-match=hidraw --action=add`.

**One OpenOCD at a time.** A second OpenOCD on the same probe, or one killed
mid-transaction, wedges the CMSIS-DAP HID stream ("CMSIS-DAP command mismatch")
and then the target looks dead ("cannot read IDR"). Check `pgrep -a openocd`
first; `swd_recover.sh` refuses to run if one is alive. To recover the probe,
kill every OpenOCD and re-authorize it: `echo 0 | sudo tee /sys/bus/usb/devices/<port>/authorized`, then `echo 1`.

## 2. The chip: nRF52840 on the XIAO nRF52840 Sense

- **Silicon:** FICR `INFO.VARIANT=0x41414430` is "AAD0", build code D0, **revision 2: legacy APPROTECT**. Protection is off unless UICR.APPROTECT=0x00. ERASEALL leaves it unlocked; it doesn't re-lock like rev 3 (Fxx) parts. CTRL-AP `APPROTECTSTATUS` (AP1 0x00C) reads 1 when unlocked.
- **CTRL-AP (AP #1):** RESET 0x000, ERASEALL 0x004, ERASEALLSTATUS 0x008, APPROTECTSTATUS 0x00C, IDR 0x0FC = 0x02880000. ERASEALL wipes flash, UICR and RAM, **without needing the core halted**. That's why recovery starts with it. OpenOCD `nrf52_recover` runs it. `nrf5 mass_erase` is different: it uses NVMC and needs a halted core (errata 184).
- **Flash layout (Adafruit bootloader):** MBR 0x0–0xB00; S140 7.3.0 0x1000–0x26498; app from 0x27000; bootloader 0xF4000 (`NRFFW[0]`); bootloader config 0xFD800; MBR params 0xFE000 (`NRFFW[1]`); **settings page 0xFF000**.
- **Settings page** (SDK11 `bootloader_settings_t`): u16 `bank_0`, u16 `bank_0_crc`, u16 `bank_1`, pad, u32 `bank_0_size`, … An app is valid only if `bank_0 == 0x01`; `crc == 0` skips the CRC check. A blank page means invalid app, so an SWD-programmed app is ignored and the bootloader stays in DFU. That's why `restore` writes `01 00 00 00 …` there (`tools/swd_image.py settings`). Upstream Adafruit PR #396 (0.11.0) accepts a plausible vector table when the page is blank; our fork predates it.
- **UICR:** the bootloader needs `NRFFW[0]=0xF4000` and `NRFFW[1]=0xFE000`. `PSELRESET[0..1]` (P0.18) and `NFCPINS` are written by the bootloader's `SystemInit` on first boot, which then resets once. Expect one SREQ reset after a fresh restore.
- **Reset diagnosis:** `RESETREAS` (0x40000400) bits: 0 pin, 1 watchdog, 2 SREQ, 3 lockup, 16+ wake sources. All zeros means power-on or brown-out. It is cumulative, write-1-to-clear, and **the bootloader never clears it**. DHCSR bit 25 `S_RESET_ST` is sticky and clears on read; if it keeps setting without you resetting anything, the core is reset-looping. Another reliable test: **halt the core**. Software can't reset a halted core, so resets that stop while it's halted are software resets.
- **Fault capture without disturbing the SoftDevice:** set `DHCSR.C_DEBUGEN` without halting. The bootloader's `app_error_fault_handler` executes `BKPT` when a debugger is attached, so the core halts right in it with `r0`=fault id, `r1`=pc, `r2`=info:
  - `0x1` = SoftDevice assert (pc is in the SoftDevice)
  - `0x1001` = `NRF_FAULT_ID_APP_MEMACC` (pc is in our code; `info` = PREGION[0] sub-region mask of the illegal write, bit *n* = peripheral at `0x40000000 + n·0x1000`)
  - `0x4001` = `APP_ERROR_CHECK` (SDK error)
- **Don't halt a running SoftDevice and resume.** It asserts on resume (timing lost). A halt-and-resume capture gives you the assert you caused.
- **Don't write POWER or CLOCK from the debugger while the SoftDevice runs** (RESETREAS, GPREGRET…). It owns them and its MWU flags writes. GPREGRET is 0x4000051C and **GPREGRET2 is 0x40000520**. 0x40000580 is `DCDCEN0`, which we once wrote by mistake; it's harmless here only because `MAINREGSTATUS=0` (normal-voltage mode, REG0 unused).

## 3. Procedure

```bash
./scripts/swd_recover.sh probe          # read-only: lock state, reset-loop check, FICR/UICR/settings
./scripts/swd_recover.sh backup DIR     # 1 MB flash + UICR + FICR dump, sha256 (needs a halt)
./scripts/swd_recover.sh restore        # backup → CTRL-AP erase → golden image + verify → reset
./scripts/swd_recover.sh restore --release b217
./scripts/swd_recover.sh restore --app path/to/app.hex --bootloader path/to/bl.uf2
```

`restore` builds the golden image with `tools/swd_image.py` from:
- S140 7.3.0 from the `bootloader/src` submodule (`git submodule update --init bootloader/src`)
- the staged fleet bootloader UF2. Only the bootloader, its config and UICR NRFFW are kept; the UF2's zero-padded UICR block is discarded.
- the latest stable release hex (`tools/firmware_release.py download`, sha256-verified)
- the settings page

`swd_image.py merge` refuses overlapping inputs. Every OpenOCD step retries,
because examine races a looping chip. Afterwards, check that the chip
enumerates (`lsusb | grep 2886:`) and let it run ≥60 s before any further
reset (CLAUDE.md, 60-second rule).

**If the chip doesn't enumerate after a good restore:**
1. `probe` should show 0 resets, and `halt; reg pc` should show a PC in the app region (≥0x27000, <0xF4000).
2. Check that the chip's own USB cable is plugged into the Pi. Power from the Debug Mate hides a missing cable.
3. After repeated enumeration failures the kernel disables the port (`usbN-portM: Cannot enable. Maybe the USB cable is bad?`). Clear it with `sudo uhubctl -l <hub> -p <port> -a cycle`, or by replugging.
4. Pi 5 onboard ports can't be power-switched one at a time. A real power-on reset of a chip needs a hub that switches each port individually, or a manual unplug of both cables (the Debug Mate also feeds the chip).

## 4. 06ACEB (blinky-firmware#12): what happened

- **Before:**
  - The chip was off the bus. The kernel logged `error -71` and `device not accepting address`, then disabled the port.
  - It carried bootloader 0.8.0-4-g76d1e60 and an **empty app region**: build b190 had been dropped as a UF2 onto 0.8.0-4, whose UF2 path silently fails to write while the SoftDevice is enabled (see `bootloader/README.md`).
  - MBR and S140 were byte-identical to the official 7.3.0 image.
- **The reset loop:** with no valid app the bootloader enters dual-transport DFU, starting both the SoftDevice and USB. About 150 ms later it reboots, every time, so USB never finishes enumerating. Fault capture (§2) on **both 0.8.0-4 and fleet 0.8.0-10** showed `id=0x1001 APP_MEMACC`, `pc=0xF99AC` (TinyUSB task code), `info=1`. The MWU registers agreed: `EVENTS_PREGION[0].WA=1`, `PERREGION[0].SUBSTATWA=0x1`. So **something writes CLOCK/POWER (0x40000000–0x40000FFF) directly while the SoftDevice is enabled**; it faults and the bootloader resets.
  - TinyUSB's HFCLK code is ruled out: it is SoftDevice-aware and built with `SOFTDEVICE_PRESENT`.
  - The exact store is not yet identified. Nordic's rule is that every CLOCK/POWER write goes through `sd_power_*` / `sd_clock_*` while the SoftDevice is enabled.
  - **Impact: the fleet bootloader reset-loops with no USB whenever the app is missing or invalid.** Its BLE side resets every ~150 ms too, so BLE recovery is doubtful as well. Bench tests always entered DFU from a running app, so they never hit this path.
- **Recovery:** CTRL-AP erase, then golden image (MBR + S140 7.3.0 + bootloader 0.8.0-10-g4c5faae + UICR + b217 + settings), verified OK. The chip boots b217 directly; `probe` shows 0 resets.
- **Traps hit:**
  - A killed OpenOCD survived and held the probe, which made the target look dead.
  - A wrong-address register write landed in DCDCEN0 (§2).
  - Halt-and-resume captures reported a SoftDevice assert that the capture itself caused.

## 5. Open items

- Find and fix the direct CLOCK/POWER write in the dual-transport DFU path. Rebasing the fork onto upstream (PR #396 at least) is also worth doing for the blank-settings-page fallback.
- Update the Debug Mate firmware (USB serial fix) so pyOCD works and several probes can be told apart.
- A per-port switchable USB hub for real per-chip power cycling in the box.

## Sources

- Seeed: [Debug Mate getting started](https://wiki.seeedstudio.com/getting_started_with_xiao_debug_mate/), [debugging](https://wiki.seeedstudio.com/xiao_debug_mate_debug/), [serial](https://wiki.seeedstudio.com/xiao_debug_mate_serial/), [OSHW repo (firmware, KiCad, `DAP_config.h`)](https://github.com/Seeed-Studio/OSHW-XIAO-Debug-Mate), issues [#1](https://github.com/Seeed-Studio/OSHW-XIAO-Debug-Mate/issues/1), [#5](https://github.com/Seeed-Studio/OSHW-XIAO-Debug-Mate/issues/5), [#6](https://github.com/Seeed-Studio/OSHW-XIAO-Debug-Mate/issues/6); [XIAO nRF52840 wiki](https://wiki.seeedstudio.com/XIAO_BLE/), [schematic](https://files.seeedstudio.com/wiki/XIAO-BLE/Res/260828_XIAO_nRF52840.pdf)
- Nordic: [IC revisions](https://docs.nordicsemi.com/r/bundle/comp_matrix_nrf52840/page/comp/nrf52840/nrf52840_ic_revision_overview.html), [FICR](https://docs.nordicsemi.com/r/bundle/ps_nrf52840/page/ficr.html), [debug/CTRL-AP/APPROTECT](https://docs.nordicsemi.com/r/bundle/ps_nrf52840/page/dif.html), [USBD](https://docs.nordicsemi.com/r/bundle/ps_nrf52840/page/usbd.html), [rev 2 errata](https://docs.nordicsemi.com/r/bundle/errata_nrf52840_rev2/page/err/nrf52840/rev2/latest/err_840.html) (136, 171, 176, 184, 187), [S140 peripheral runtime protection](https://docs.nordicsemi.com/r/bundle/sds_s140/page/sds/s1xx/sd_resource_reqs/peripheral_runtime_protection.html), [nrf52840.svd](https://github.com/NordicSemiconductor/nrfx/blob/master/bsp/stable/mdk/nrf52/nrf52840/nrf52840.svd); DevZone [30856 (GPREGRET MEMACC)](https://devzone.nordicsemi.com/f/nordic-q-a/30856/invalid-memory-access-when-writing-to-nrf_power--gpregret), [77101 (SUBSTATWA=0x1 with TinyUSB)](https://devzone.nordicsemi.com/f/nordic-q-a/77101/got-softdevice-invalid-memory-access-error-on-nrf52840/318563)
- Adafruit bootloader: [repo](https://github.com/adafruit/Adafruit_nRF52_Bootloader) (Makefile `flash`/`flash-sd`/`FLASHER=pyocd`, `bootloader.c`, `main.c`, HIL contract `.claude/skills/hil-contract/SKILL.md`), [PR #396](https://github.com/adafruit/Adafruit_nRF52_Bootloader/pull/396), issue [#290](https://github.com/adafruit/Adafruit_nRF52_Bootloader/issues/290)
- Tools: [OpenOCD nrf52.cfg (`nrf52_recover`)](https://github.com/openocd-org/openocd/blob/master/tcl/target/nordic/nrf52.cfg), [OpenOCD nrf5.c](https://github.com/openocd-org/openocd/blob/master/src/flash/nor/nrf5.c), [pyOCD nRF52 target](https://github.com/pyocd/pyOCD/blob/main/pyocd/target/family/target_nRF52.py), [probe-rs nrf52 sequence](https://github.com/probe-rs/probe-rs/blob/master/probe-rs/src/vendor/nordicsemi/sequences/nrf52.rs), [uhubctl](https://github.com/mvp/uhubctl), [Linux USB error codes](https://www.kernel.org/doc/html/latest/driver-api/usb/error-codes.html)
