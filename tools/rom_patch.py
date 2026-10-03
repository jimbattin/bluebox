#!/usr/bin/env -S uv run
"""Write a copy of the ROM set with patched bytes, keeping every checksum valid.

Usage: rom_patch.py OUTDIR ADDR=VAL [ADDR=VAL ...] [--fix ADDR]

ADDR and VAL are hex 8080 addresses and bytes (0000-17FF SBC, 8000-FFFF NVS).
Each ROM's byte sum must stay equal to its socket index (docs/rom_map.md), so
every patched ROM gets one compensation byte. The default is the ROM's
checksum-adjust byte where one is known: 0003 in SBC1 and FFFE in NVS8, which
covers the preset blocks at F000. Other ROMs need --fix ADDR (a byte inside
that ROM that the firmware does not use). The output directory then works with
`lex224emu --rom-dir OUTDIR` and `lexprobe --rom-dir OUTDIR`.
"""

import sys
from pathlib import Path

from rom_map import ROM_DIR, ROMS

KNOWN_FIX = {"SBC1": 0x0003, "NVS8": 0xFFFE}


def rom_of(addr):
    for name, base, size, index in ROMS:
        if base <= addr < base + size:
            return name, base, size, index
    sys.exit(f"address {addr:04X} is not in a ROM")


def main(argv):
    if len(argv) < 2 or argv[0].startswith("-"):
        sys.exit(__doc__)
    out = Path(argv[0])
    patches, fixes = {}, []
    args = iter(argv[1:])
    try:
        for s in args:
            if s == "--fix":
                fixes.append(int(next(args), 16))
            else:
                a, v = s.split("=")
                patches[int(a, 16)] = int(v, 16) & 0xFF
    except (ValueError, StopIteration):
        sys.exit(__doc__)

    images = {name: bytearray((ROM_DIR / name).read_bytes()) for name, *_ in ROMS}
    touched = {}
    for addr, v in patches.items():
        name, base, _, _ = rom_of(addr)
        images[name][addr - base] = v
        touched[name] = None
    for addr in fixes:
        name, base, _, _ = rom_of(addr)
        if addr in patches:
            sys.exit(f"--fix {addr:04X} is itself patched")
        touched[name] = addr
    for name, fix in touched.items():
        _, base, _, index = next(r for r in ROMS if r[0] == name)
        tag = name.split()[0]
        if fix is None:
            fix = KNOWN_FIX.get(tag)
            if fix is None:
                sys.exit(f"{tag} has no known checksum-adjust byte: pass --fix ADDR inside it")
            if fix in patches:
                sys.exit(f"{tag}'s checksum-adjust byte {fix:04X} is patched: pass --fix ADDR")
        img = images[name]
        img[fix - base] = (img[fix - base] + index - sum(img)) & 0xFF
        assert sum(img) & 0xFF == index
        print(f"{tag}: compensation byte {fix:04X} = {img[fix - base]:02X}")

    out.mkdir(parents=True, exist_ok=True)
    for name, *_ in ROMS:
        (out / name).write_bytes(images[name])


if __name__ == "__main__":
    main(sys.argv[1:])
