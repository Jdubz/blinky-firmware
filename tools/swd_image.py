#!/usr/bin/env python3
"""
swd_image.py - build the images scripts/swd/swd_recover.sh programs over SWD.

Standard library only (the test box stays light). Subcommands:

  bootloader UF2 OUT.hex [--with-mbr]
      Convert a bootloader UF2 (e.g. bootloader/update-bootloader-qspi-ota-default.uf2,
      family 0xd663823c) into an Intel HEX of the bootloader (0xF4000..) and its
      config block (0xFD800). The UF2's UICR block is padded with zeros by
      uf2conv; only NRFFW[0] (0x10001014, bootloader address) and NRFFW[1]
      (0x10001018, MBR params page) carry meaning, so only those two words are
      kept. The MBR (0x0..0xB00) is left out unless --with-mbr: the SoftDevice
      hex already carries the same MBR, and a bootloader-only restore must not
      touch it.

  settings OUT.hex --app-size N
      The bootloader settings page (0xFF000) marking bank 0 as a valid app
      (bank_0 = BANK_VALID_APP, bank_0_crc = 0 = "skip the CRC check"). Without
      it the bootloader treats an SWD-programmed app as invalid and stays in
      DFU (lib/sdk11/.../bootloader.c, bootloader_app_is_valid()).

  merge OUT.hex IN.hex...
      Merge Intel HEX files into one golden image. Overlapping bytes are an
      error, so a wrong input can't silently overwrite another.

  hexinfo FILE.hex
      Print the address ranges of an Intel HEX (and the app size from 0x27000).
"""

import argparse
import struct
import sys

UF2_MAGIC0, UF2_MAGIC1, UF2_MAGIC_END = 0x0A324655, 0x9E5D5157, 0x0AB16F30
APP_START = 0x27000
SETTINGS_ADDR = 0xFF000
UICR_KEEP = (0x10001014, 0x10001018)  # NRFFW[0], NRFFW[1]


def read_uf2(path):
    data = open(path, "rb").read()
    if len(data) % 512:
        sys.exit(f"{path}: not a UF2 (size {len(data)} is not a multiple of 512)")
    mem = {}
    for off in range(0, len(data), 512):
        m0, m1, _flags, addr, size, _no, _n, _fam = struct.unpack_from("<8I", data, off)
        if (m0, m1) != (UF2_MAGIC0, UF2_MAGIC1) or struct.unpack_from("<I", data, off + 508)[0] != UF2_MAGIC_END:
            sys.exit(f"{path}: bad UF2 magic in block at offset {off}")
        for i, b in enumerate(data[off + 32: off + 32 + size]):
            mem[addr + i] = b
    return mem


def read_hex(path):
    mem, base = {}, 0
    for n, line in enumerate(open(path), 1):
        line = line.strip()
        if not line:
            continue
        if not line.startswith(":"):
            sys.exit(f"{path}:{n}: not Intel HEX")
        raw = bytes.fromhex(line[1:])
        if sum(raw) & 0xFF:
            sys.exit(f"{path}:{n}: checksum error")
        count, addr, rtype, payload = raw[0], (raw[1] << 8) | raw[2], raw[3], raw[4:4 + raw[0]]
        if rtype == 0:
            for i, b in enumerate(payload):
                mem[base + addr + i] = b
        elif rtype == 1:
            break
        elif rtype == 2:
            base = int.from_bytes(payload, "big") << 4
        elif rtype == 4:
            base = int.from_bytes(payload, "big") << 16
    return mem


def write_hex(mem, path):
    out, upper = [], None

    def rec(rtype, addr, payload):
        body = bytes([len(payload), (addr >> 8) & 0xFF, addr & 0xFF, rtype]) + payload
        out.append(":" + (body + bytes([(-sum(body)) & 0xFF])).hex().upper())

    addrs = sorted(mem)
    i = 0
    while i < len(addrs):
        start = addrs[i]
        chunk = bytearray([mem[start]])
        i += 1
        while i < len(addrs) and addrs[i] == start + len(chunk) and len(chunk) < 16 \
                and (start & 0xFFFF) + len(chunk) <= 0xFFFF:
            chunk.append(mem[addrs[i]])
            i += 1
        if start >> 16 != upper:
            upper = start >> 16
            rec(4, 0, upper.to_bytes(2, "big"))
        rec(0, start & 0xFFFF, bytes(chunk))
    rec(1, 0, b"")
    open(path, "w").write("\n".join(out) + "\n")


def ranges(mem):
    out, addrs = [], sorted(mem)
    for a in addrs:
        if out and out[-1][1] == a:
            out[-1][1] = a + 1
        else:
            out.append([a, a + 1])
    return out


def cmd_bootloader(args):
    mem = read_uf2(args.uf2)
    keep = {}
    for a, b in mem.items():
        if a < 0x1000:
            if args.with_mbr:
                keep[a] = b
        elif 0xF4000 <= a < 0xFE000:
            keep[a] = b
        elif any(w <= a < w + 4 for w in UICR_KEEP):
            keep[a] = b
    if not any(0xF4000 <= a < 0xFC000 for a in keep):
        sys.exit(f"{args.uf2}: no bootloader data at 0xF4000 - is this a bootloader UF2?")
    for w in UICR_KEEP:
        v = int.from_bytes(bytes(keep.get(w + i, 0xFF) for i in range(4)), "little")
        print(f"UICR {w:#010x} = {v:#010x}")
    write_hex(keep, args.out)
    for s, e in ranges(keep):
        print(f"  {s:#010x}..{e:#010x} ({e - s} bytes)")


def cmd_settings(args):
    # bootloader_settings_t (bootloader_types.h): u16 bank_0, u16 bank_0_crc,
    # u16 bank_1, [pad], u32 bank_0_size, sd/bl/app_image_size, sd_image_start.
    page = struct.pack("<HHHHI", 0x01, 0x0000, 0xFF, 0xFFFF, args.app_size)
    write_hex({SETTINGS_ADDR + i: b for i, b in enumerate(page)}, args.out)
    print(f"settings page {SETTINGS_ADDR:#x}: bank_0=VALID_APP crc=skip size={args.app_size}")


def cmd_merge(args):
    mem = {}
    for path in args.inputs:
        part = read_hex(path)
        clash = set(part) & set(mem)
        if clash:
            sys.exit(f"{path} overlaps earlier inputs at {min(clash):#x}")
        mem.update(part)
    write_hex(mem, args.out)
    for s, e in ranges(mem):
        print(f"  {s:#010x}..{e:#010x}")


def cmd_hexinfo(args):
    mem = read_hex(args.hex)
    for s, e in ranges(mem):
        print(f"{s:#010x}..{e:#010x} ({e - s} bytes)")
    app = [a for a in mem if a >= APP_START and a < 0xF4000]
    if app:
        print(f"app_size={max(app) + 1 - APP_START}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("bootloader")
    b.add_argument("uf2")
    b.add_argument("out")
    b.add_argument("--with-mbr", action="store_true")
    s = sub.add_parser("settings")
    s.add_argument("out")
    s.add_argument("--app-size", type=int, required=True)
    m = sub.add_parser("merge")
    m.add_argument("out")
    m.add_argument("inputs", nargs="+")
    h = sub.add_parser("hexinfo")
    h.add_argument("hex")
    args = p.parse_args()
    {"bootloader": cmd_bootloader, "settings": cmd_settings, "merge": cmd_merge,
     "hexinfo": cmd_hexinfo}[args.cmd](args)


if __name__ == "__main__":
    main()
