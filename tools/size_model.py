#!/usr/bin/env -S uv run
"""Model of the SIZE offset mapping (NVS4 B55B-B714), checked against the emulator.

Usage: size_model.py [--lexprobe PATH] [ID ...]

For each program ID (hex; default: every factory program), boots the emulator
at several SIZE values through `lexprobe` and compares every memory-op WCS
offset with S(template offset) from this model. Programs whose record byte 30h
is FF have no SIZE map; the check then confirms that SIZE moves no offset.

Steps the firmware rewrites after the image copy are listed separately:
type-2 delay steps must come out as S(base) + d + 1 with d the same at every
SIZE. Anything else (the MODE ENH taps) is reported as "other". The model's
anchors and stretch factors are also compared with the firmware's 3CE5-3CEA.
See docs/parameters.md §5.2.
"""

import subprocess
import sys
from pathlib import Path

from rom_map import load_image, preset_blocks, program_table

M16 = 0xFFFF
SIZES = (0x00, 0x01, 0x20, 0x40, 0x80, 0xC0, 0xFE, 0xFF)
DEFAULT_PROBE = Path(__file__).resolve().parent.parent / "emu" / "build" / "lexprobe"


def div11CA(hl, bc):
    """SBC3 11CA: 16 steps of HL<<1 against BC; quotient bits into DE."""
    de = 0
    for _ in range(16):
        de = (de << 1) & M16
        carry = hl >> 15
        hl = (hl << 1) & M16
        if carry or hl >= bc:
            hl = (hl - bc) & M16
            de |= 1
    return de


def stretch(lo, size, hi):
    """B60C: floor(16 * (lo + floor(size * (hi - lo) / 256)) / lo), 1/16 units."""
    l = (lo + ((size * ((hi - lo) & 0xFF)) >> 8)) & 0xFF
    q = div11CA(l, (lo << 8) & M16)
    return (((q << 4) & M16) >> 8) & 0xFF


def lerp(x0, x1, r):
    """B6FA: x0 + (r * (x1 - x0)) >> 4, 16-bit."""
    return (x0 + (((r * ((x1 - x0) & M16)) >> 4) & M16)) & M16


class SizeMap:
    """S(x) from the 13 bytes at 3CD8-3CE4 (record bytes 36h-42h)."""

    def __init__(self, st, size1, size2):
        w = lambda i: st[i] | st[i + 1] << 8
        self.T0, self.T1, self.T2, self.T3 = w(0), w(2), w(4), w(6)
        f, lo, hi, lo2, hi2 = st[8:13]
        self.r1 = stretch(lo, size1, hi)
        self.E5 = (lerp(self.T0, self.T1, self.r1) + 1) & M16
        self.E7 = (self.E5 + ((-self.E5 & M16) >> 1)) & M16
        if f == 0:
            self.r2 = stretch(lo2, size2, hi2)
        else:
            r2 = (((((f * ((self.r1 - 16) & 0xFF)) << 2) & M16) >> 8) + 16) & 0xFF
            cap = div11CA(((self.T0 - self.T2) & M16) >> 2, (((self.T3 - self.T2) & M16) << 2) & M16) >> 8
            self.r2 = min(r2, cap)

    def S(self, x):
        T0, T1, T2 = self.T0, self.T1, self.T2
        if T2 >= x:
            return x
        if x <= (T0 - 1) & M16:
            return lerp(T2, x, self.r2)
        if x <= T1:
            return lerp(T0, x, self.r1)
        m = (T1 + ((-T1 & M16) >> 1)) & M16
        if x <= (m - 1) & M16:
            return (x - T1 + self.E5) & M16
        return (x - m + self.E7) & M16


def probe(exe, *args):
    return subprocess.run([str(exe), *args], capture_output=True, text=True, check=True).stdout


def boot(exe, pid, patches):
    """(SIZE, SIZE 2, 3CD8-3CEA bytes, [(template, op, wcs offset)] for steps 0..127)."""
    p = [f"{a:04X}={v:02X}" for a, v in patches]
    mem = [l.split()[1:] for l in probe(exe, "mem", pid, *p, "3CCE:2", "3CD8:13").splitlines()]
    s1, s2 = (int(x, 16) for x in mem[0])
    st = [int(x, 16) for x in mem[1]]
    steps = [None] * 128
    for line in probe(exe, "steps", pid, *p).splitlines()[2:]:
        f = line.split()
        steps[int(f[0])] = (int(f[4], 16), f[3], int(f[5], 16))
    return s1, s2, st, steps


def check(exe, pid, block, split, record_fe):
    runs = {}
    for size in SIZES:
        patches = [(block + 0x30, size)] + ([(block + 0x31, size)] if split else [])
        runs[size] = boot(exe, pid, patches)
    mem_steps = [n for n, (_, op, _) in enumerate(runs[0][3]) if op in ("MR", "MW")]
    if not record_fe:
        moved = [n for n in mem_steps if len({runs[s][3][n][2] for s in SIZES}) > 1]
        return f"FF record: {len(moved)} memory-op offsets change with SIZE"
    state_bad = 0  # the firmware's own 3CE5/3CE7 anchors and 3CE9/3CEA factors
    for s1, s2, st, _ in runs.values():
        sm = SizeMap(st, s1, s2)
        state_bad += (sm.E5, sm.E7, sm.r1, sm.r2) != (st[13] | st[14] << 8, st[15] | st[16] << 8, st[17], st[18])
    ok = rewritten = 0
    other = set()
    for n in mem_steps:
        good = all(SizeMap(st, s1, s2).S(steps[n][0]) == steps[n][2] for s1, s2, st, steps in runs.values())
        if good:
            ok += 1
            continue
        ds = {(steps[n][2] - SizeMap(st, s1, s2).S(steps[n][0]) - 1) & M16 for s1, s2, st, steps in runs.values()}
        if len(ds) == 1:
            rewritten += 1
        else:
            other.add(n)
    fe = SizeMap(runs[0xFE][2], 0xFE, 0xFE)
    return (f"FE record: {ok} offsets = S(template), {rewritten} = S(base)+d+1 (d fixed), "
            f"other {sorted(other)}; 3CE5-3CEA mismatches {state_bad}; stretch at FE x{fe.r1 / 16:.3f} / x{fe.r2 / 16:.3f}")


def main(argv):
    exe = DEFAULT_PROBE
    if argv[:1] == ["--lexprobe"]:
        exe, argv = Path(argv[1]), argv[2:]
    if any(a.startswith("-") for a in argv):
        sys.exit(__doc__)
    if not exe.is_file():
        sys.exit(f"lexprobe not found at {exe}: build emu/ first (emu/README.md)")
    mem, _ = load_image()
    blocks = {pid: addr for addr, pid, _, _ in preset_blocks(mem)}
    ids = [int(a, 16) for a in argv] or [entry[0] for entry in program_table(mem)]
    for pid in ids:
        rec = int(probe(exe, "steps", f"{pid:02X}").split()[3], 16)
        record_fe = mem[rec + 0x30] == 0xFE
        split = mem[rec + 0x31] == 0xFE
        print(f"{pid:02X} record {rec:04X}: {check(exe, f'{pid:02X}', blocks[pid], split, record_fe)}")


if __name__ == "__main__":
    main(sys.argv[1:])
