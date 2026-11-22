#!/bin/bash
# swd_recover.sh — SWD recovery for an nRF52840 test chip (docs/SWD_RECOVERY.md).
#
# Brings a bricked chip back to a known-good image over SWD, using the
# community-standard sequence (Adafruit bootloader HIL contract, OpenOCD
# nrf52.cfg): CTRL-AP ERASEALL -> program a golden image (MBR + S140 +
# bootloader + UICR + app + settings) with verify -> reset. The CTRL-AP erase
# needs no core halt, so it works on a chip stuck in a fast reset loop.
#
# The probe is a Seeed XIAO Debug Mate (CMSIS-DAP v1 HID). It has no working
# nRESET line, so every reset is SYSRESETREQ or CTRL-AP. pyOCD can't open it
# (the probe reports an empty USB serial); OpenOCD can.
#
# Usage:
#   ./scripts/swd_recover.sh probe                      # identify chip, lock state, reset-loop check
#   ./scripts/swd_recover.sh backup DIR                 # dump flash (1 MB) + UICR, read-only
#   ./scripts/swd_recover.sh restore [--app HEX | --release bN] [--bootloader UF2] [--no-backup]
#
# restore defaults: the staged fleet bootloader (bootloader/update-bootloader-qspi-ota-default.uf2),
# S140 7.3.0 from the bootloader/src submodule, and the latest STABLE release
# app (tools/firmware_release.py download). Always backs up first unless
# --no-backup. The bootloader settings page is written as "valid app, skip CRC"
# so the bootloader jumps to the app instead of entering DFU.
#
# Test chips only: this mass-erases the chip.

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SD_HEX="$REPO/bootloader/src/lib/softdevice/s140_nrf52_7.3.0/s140_nrf52_7.3.0_softdevice.hex"
BL_UF2="$REPO/bootloader/update-bootloader-qspi-ota-default.uf2"
OCD=(openocd -f interface/cmsis-dap.cfg -c "transport select swd" -c "adapter speed 1000"
     -f target/nordic/nrf52.cfg)
TRIES=5

die() { echo "ERROR: $*" >&2; exit 1; }

# OpenOCD's examine races a reset-looping chip, so every step retries.
ocd() {
    local log rc
    log=$(mktemp)
    for i in $(seq 1 "$TRIES"); do
        if timeout 600 "${OCD[@]}" "$@" -c shutdown >"$log" 2>&1; then rc=0; else rc=$?; fi
        if [ $rc -eq 0 ] && ! grep -q "Examination failed\|Target not examined" "$log"; then
            cat "$log"; rm -f "$log"; return 0
        fi
        echo "  (openocd attempt $i/$TRIES failed, retrying)" >&2
    done
    cat "$log" >&2; rm -f "$log"
    return 1
}

check_tools() {
    command -v openocd >/dev/null || die "openocd not installed (sudo apt install openocd)"
    if pgrep -x openocd >/dev/null; then
        die "another openocd is running (pid $(pgrep -x openocd | tr '\n' ' ')); two sessions on one probe wedge it"
    fi
}

cmd_probe() {
    # No halt: only reads, plus a 2 s DHCSR poll for S_RESET_ST (sticky "core was reset").
    ocd -c "nrf52.cpu configure -defer-examine" -c init -c '
        echo "CTRL-AP IDR=[nrf52.dap apreg 1 0xfc] APPROTECTSTATUS=[nrf52.dap apreg 1 0xc] (1 = unlocked)"
        proc rd1 {a} { nrf52.dap apreg 0 0x00 0x23000002; nrf52.dap apreg 0 0x04 $a; return [nrf52.dap apreg 0 0x0c] }
        proc rd {a} { for {set k 0} {$k < 3} {incr k} { if {![catch {set v [rd1 $a]}]} {return $v} }; error "read $a failed" }
        catch {rd 0xE000EDF0}
        set resets 0; set errs 0
        for {set i 0} {$i < 40} {incr i} { if {[catch {set d [rd 0xE000EDF0]}]} {incr errs} elseif {$d & 0x02000000} {incr resets}; sleep 50 }
        echo "reset-loop check: $resets resets, $errs failed reads in ~2 s (0/0 = stable)"
        foreach {n a} {PART 0x10000100 VARIANT 0x10000104 DEVICEID0 0x10000060 DEVICEID1 0x10000064 NRFFW0 0x10001014 NRFFW1 0x10001018 PSELRESET0 0x10001200 APPROTECT 0x10001208 SETTINGS 0x000FF000} {
            if {[catch {echo [format "%-10s %s" $n [rd $a]]}]} {echo "$n read failed"}
        }' | grep -v -E "^(Open On-Chip|Licensed|For bug|\s+http|Info :)"
}

cmd_backup() {
    local dir="$1"
    mkdir -p "$dir"
    dir="$(cd "$dir" && pwd)"
    ocd -c init -c "reset halt" \
        -c "dump_image $dir/flash.bin 0x0 0x100000" \
        -c "dump_image $dir/uicr.bin 0x10001000 0x400" \
        -c "dump_image $dir/ficr.bin 0x10000000 0x400" >/dev/null
    [ "$(stat -c %s "$dir/flash.bin")" -eq 1048576 ] || die "flash dump is short (target reset mid-read?) — retry"
    (cd "$dir" && sha256sum flash.bin uicr.bin ficr.bin > SHA256SUMS)
    echo "backup written to $dir"
}

cmd_restore() {
    local app="" release="" backup=1
    while [ $# -gt 0 ]; do
        case "$1" in
            --app) app="$2"; shift 2 ;;
            --release) release="$2"; shift 2 ;;
            --bootloader) BL_UF2="$2"; shift 2 ;;
            --no-backup) backup=0; shift ;;
            *) die "unknown restore option: $1" ;;
        esac
    done
    [ -f "$SD_HEX" ] || die "missing $SD_HEX (git submodule update --init bootloader/src)"
    [ -f "$BL_UF2" ] || die "missing bootloader UF2 $BL_UF2"

    local work
    work="$(mktemp -d /tmp/swd_recover.XXXXXX)"
    if [ -z "$app" ]; then
        local args=(download --out "$work/release")
        [ -n "$release" ] && args+=(--tag "$release")
        python3 "$REPO/tools/firmware_release.py" "${args[@]}" > "$work/release.json"
        app="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["hex"])' "$work/release.json")"
    fi
    [ -f "$app" ] || die "missing app hex $app"

    echo "== building golden image in $work"
    python3 "$REPO/tools/swd_image.py" bootloader "$BL_UF2" "$work/bl.hex"
    local size
    size="$(python3 "$REPO/tools/swd_image.py" hexinfo "$app" | sed -n 's/^app_size=//p')"
    [ -n "$size" ] || die "$app has no data in the app region (0x27000..)"
    python3 "$REPO/tools/swd_image.py" settings "$work/settings.hex" --app-size "$size"
    python3 "$REPO/tools/swd_image.py" merge "$work/golden.hex" "$SD_HEX" "$work/bl.hex" "$app" "$work/settings.hex"

    if [ $backup -eq 1 ]; then
        echo "== backup"
        # Subshell: cmd_backup's die must fail only the backup, not the restore.
        ( cmd_backup "$work/backup" ) \
            || echo "WARNING: backup failed (chip may be looping too fast to halt); continuing" >&2
    fi

    echo "== CTRL-AP mass erase"
    ocd -c "nrf52.cpu configure -defer-examine" -c init -c nrf52_recover | grep -E "erased|recovery failed|Cannot access" \
        | tee "$work/erase.log"
    grep -q "successfully erased" "$work/erase.log" || die "CTRL-AP erase did not report success"

    echo "== program + verify"
    ocd -c init -c "reset halt" -c "program $work/golden.hex verify" > "$work/program.log"
    grep -q "Verified OK" "$work/program.log" || { cat "$work/program.log" >&2; die "verify failed"; }

    echo "== reset"
    ocd -c init -c "reset run" >/dev/null
    echo "restored. Wait ~5 s, then check: lsusb | grep 2886: (needs the chip's own USB cable)"
    echo "work dir (golden.hex, backup): $work"
}

check_tools
case "${1:-}" in
    probe) cmd_probe ;;
    backup) [ $# -eq 2 ] || die "usage: $0 backup DIR"; cmd_backup "$2" ;;
    restore) shift; cmd_restore "$@" ;;
    *) sed -n '2,25p' "$0"; exit 1 ;;
esac
