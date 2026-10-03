#!/usr/bin/env -S uv run
"""Map and cross-check the Lexicon 224XL v8.2 ROM set.

Loads the 11 EPROM images into the 8080 address space and walks the tables the
firmware itself uses (SBC3 routines 133E, 1361, 12B7; factory table at A446).
Exits 1 if a ROM checksum fails or a factory program does not resolve to a
microcode record. See docs/rom_map.md.
"""

import sys
from pathlib import Path

ROM_DIR = Path(__file__).resolve().parent.parent / "roms" / "224XL_v8_21"

# (filename, base address, size, checksum index)
ROMS = [
    ("SBC1 2716.BIN", 0x0000, 0x800, 1),
    ("SBC2 2716.BIN", 0x0800, 0x800, 2),
    ("SBC3 2716.BIN", 0x1000, 0x800, 3),
] + [(f"NVS{n} 2732.BIN", 0x8000 + (n - 1) * 0x1000, 0x1000, n) for n in range(1, 9)]

RECORD_BASE = 0xB800
RECORD_SIZE = 0x2AA
RECORD_SLOTS = 21
PRESET_BASE = 0xF000
PROGRAM_TABLE = 0xA446
LABEL_POINTERS = 0x98F6
LABEL_WIDTH = 12


def load_image():
    """Return (64 KiB memory image, [(start, end, rom name)])."""
    mem = bytearray(b"\xff" * 0x10000)
    owners = []
    for name, base, size, _ in ROMS:
        path = ROM_DIR / name
        if not path.is_file() or path.stat().st_size != size:
            sys.exit(f"missing or wrong-size ROM: {path}")
        mem[base:base + size] = path.read_bytes()
        owners.append((base, base + size - 1, name.split()[0]))
    return mem, owners


def owner(owners, addr):
    for start, end, name in owners:
        if start <= addr <= end:
            return name
    return "-"


def checksums(mem, roms=ROMS):
    """[(name, 8-bit sum, expected index, ok)]; the firmware expects sum == index."""
    out = []
    for name, base, size, index in roms:
        s = sum(mem[base:base + size]) & 0xFF
        out.append((name, s, index, s == index))
    return out


def record_slots(mem):
    """[(slot, addr, microcode id)] mirroring SBC3 133E: +2AAh, skip xxFE."""
    out = []
    addr = RECORD_BASE
    for slot in range(RECORD_SLOTS):
        out.append((slot, addr, mem[addr]))
        addr += RECORD_SIZE
        if addr & 0xFF == 0xFE:
            addr += 2
    return out


def preset_blocks(mem):
    """[(addr, program id, length, microcode id)] mirroring SBC3 1361."""
    out = []
    addr = PRESET_BASE
    while True:
        if addr > 0xFFFF:
            raise ValueError(f"preset chain ran past FFFF (at {addr:05X})")
        pid = mem[addr]
        if pid == 0xFF:
            return out
        if addr + 3 > 0xFFFF:
            raise ValueError(f"truncated preset block at {addr:04X}")
        length = mem[addr + 1] | mem[addr + 2] << 8
        out.append((addr, pid, length, mem[addr + 3]))
        addr += 2 + length


def program_table(mem):
    """[(program id, bank, program #, label idx, layout idx)] from A446."""
    out = []
    addr = PROGRAM_TABLE
    while mem[addr] != 0xFF:
        out.append(tuple(mem[addr:addr + 5]))
        addr += 5
    return out


def label(mem, table, idx):
    """12-char label `idx` of label table `table` (0 -> 98FA, 1 -> A1B2)."""
    p = LABEL_POINTERS + 2 * table
    base = mem[p] | mem[p + 1] << 8
    a = base + LABEL_WIDTH * idx
    return mem[a:a + LABEL_WIDTH].decode("ascii", "replace").rstrip()


def ff_runs(mem, start, end, minlen=16):
    """[(start, end)] inclusive runs of FF within [start, end] of length >= minlen."""
    out = []
    run = None
    for a in range(start, end + 1):
        if mem[a] == 0xFF:
            if run is None:
                run = a
        else:
            if run is not None and a - run >= minlen:
                out.append((run, a - 1))
            run = None
    if run is not None and end + 1 - run >= minlen:
        out.append((run, end))
    return out


def main():
    mem, owners = load_image()
    ok = True

    print("== Checksums")
    for name, s, index, good in checksums(mem):
        ok &= good
        print(f"{name:14} sum={s:02X} expect={index:02X} {'OK' if good else 'MISMATCH'}")

    print("\n== Microcode record slots")
    slots = record_slots(mem)
    slot_by_id = {}
    for slot, addr, mid in slots:
        if mid == 0xFF:
            print(f"#{slot:<2} @{addr:04X} {owner(owners, addr)} id=FF (empty)")
        else:
            slot_by_id.setdefault(mid, (slot, addr))
            print(f"#{slot:<2} @{addr:04X} {owner(owners, addr)} id={mid:02X}")

    print("\n== Preset blocks")
    presets = preset_blocks(mem)
    preset_by_id = {}
    for addr, pid, length, mid in presets:
        preset_by_id.setdefault(pid, mid)
        print(f"@{addr:04X} prog={pid:02X} len={length:04X} microcode={mid:02X}")
    print(f"{len(presets)} preset blocks")

    programs = program_table(mem)
    known = {p[0] for p in programs}
    for addr, pid, length, mid in presets:
        if pid not in known:
            note = "" if mid in slot_by_id else " (no record)"
            print(f"orphan preset {pid:02X} -> microcode {mid:02X}{note}")

    print("\n== Factory programs")
    for pid, bank, prog, lbl, layout in programs:
        name = label(mem, 0, lbl)
        head = f"B{bank} P{prog} {name:12} prog={pid:02X} layout={layout:X}"
        mid = preset_by_id.get(pid)
        if mid is None:
            ok = False
            print(f"{head} UNRESOLVED: no preset block")
            continue
        if mid not in slot_by_id:
            ok = False
            print(f"{head} microcode={mid:02X} UNRESOLVED: no record slot")
            continue
        slot, addr = slot_by_id[mid]
        print(f"{head} microcode={mid:02X} slot=#{slot} @{addr:04X} {owner(owners, addr)}")

    print("\n== FF fill regions")
    for name, base, size, _ in ROMS:
        for start, end in ff_runs(mem, base, base + size - 1):
            print(f"{name.split()[0]:5} {start:04X}-{end:04X} ({end - start + 1} bytes)")

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
