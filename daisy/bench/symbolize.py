#!/usr/bin/env -S uv run
"""Aggregate lexbench's `H addr count` profile lines by function.

lexbench (daisy/bench/bench.cpp) prints a PC-sampling histogram over USB
serial as `H addr count` lines, one per 8-byte bucket, plus one `S samples N
other M` totals line (see bench.cpp's header comment for the full log
format). This script maps each bucket's address to the enclosing function,
using the addresses and sizes from `arm-none-eabi-nm` on the built ELF, and
prints the percentage of samples landing in each function, sorted
descending.

Usage:
    uv run daisy/bench/symbolize.py ELF LOG

    ELF  path to the built lexbench.elf (daisy/bench/build/lexbench.elf)
    LOG  a text file with the captured serial log (e.g. from
         `daisy/tools/cdc_read.py N > LOG`), containing at least one `H`
         line and the `S` totals line.

Requires `arm-none-eabi-nm` on PATH (see daisy/README.md for the toolchain
setup: `export PATH=$HOME/.arm/arm-gnu-toolchain-.../bin:$PATH`).
"""
import bisect
import re
import subprocess
import sys


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit(f'usage: {sys.argv[0]} ELF LOG')
    elf, log = sys.argv[1], sys.argv[2]
    syms = []
    for line in subprocess.run(['arm-none-eabi-nm', '-C', '-S', '--defined-only', elf],
                                capture_output=True, text=True, check=True).stdout.splitlines():
        m = re.match(r'([0-9a-f]+) ([0-9a-f]+) [tTwW] (.*)', line)
        if m:
            syms.append((int(m[1], 16), int(m[2], 16), m[3]))
    syms.sort()
    starts = [s[0] for s in syms]
    hist, total, other = {}, None, 0
    for line in open(log):
        m = re.search(r' H ([0-9a-f]{8}) (\d+)', line)
        if m:
            hist[int(m[1], 16)] = int(m[2])  # repeats overwrite with identical values
        m = re.search(r' S samples (\d+) other (\d+)', line)
        if m:
            total, other = int(m[1]), int(m[2])
    if total is None:
        sys.exit('no "S samples N other M" line found in LOG')
    byfn = {}
    for a, c in hist.items():
        i = bisect.bisect_right(starts, a) - 1
        name = syms[i][2] if i >= 0 and a < syms[i][0] + max(syms[i][1], 1) else '?'
        byfn[name] = byfn.get(name, 0) + c
    covered = sum(hist.values())
    print(f'samples {total}, top buckets cover {covered} ({100 * covered / total:.1f}%), other {other}')
    for n, c in sorted(byfn.items(), key=lambda x: -x[1]):
        print(f'{100 * c / total:6.2f}%  {n[:110]}')


if __name__ == '__main__':
    main()
