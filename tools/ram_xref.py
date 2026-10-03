#!/usr/bin/env -S uv run
"""Cross-reference RAM and I/O port use in the Lexicon 224XL v8.2 ROM set.

Lists every direct reference (LDA/STA/LHLD/SHLD/LXI) to the NVS RAM, SBC RAM
and DSP program RAM windows, and every IN/OUT port instruction, found by a
linear 8080 sweep of the known code regions. The sweep does not follow control
flow, so inline data inside a region can be misread as instructions and produce
false hits.
"""

import sys

from rom_map import load_image, owner

# Inclusive (start, end). Adjacent code is merged so no instruction straddles a
# boundary. SBC2 0800-0863 is excluded: an origin-0 fragment.
CODE_REGIONS = [
    (0x0000, 0x0002),  # SBC1 reset JMP 003Bh
    (0x0038, 0x00D7),  # RST 7 vector + reset/USART init
    (0x021A, 0x0768),  # SBC1 diagnostics/utility code
    (0x0864, 0x102A),  # SBC2 tests running into SBC3
    (0x10AB, 0x16B8),  # SBC3 code after the word tables
    (0x8000, 0x98F5),  # NVS1 + NVS2 code
    (0xA791, 0xAFFF),  # NVS3 code
    (0xB000, 0xB71A),  # NVS4 code
]

RAM_WINDOWS = [
    ("NVS RAM", 0x2000, 0x27FF),
    ("SBC RAM", 0x3C00, 0x3FFF),
    ("DSP program RAM", 0x4000, 0x41FF),
]

_LEN3 = {
    0x01, 0x11, 0x21, 0x31, 0x22, 0x2A, 0x32, 0x3A, 0xC3, 0xCD,
    0xC2, 0xCA, 0xD2, 0xDA, 0xE2, 0xEA, 0xF2, 0xFA,
    0xC4, 0xCC, 0xD4, 0xDC, 0xE4, 0xEC, 0xF4, 0xFC,
    0xCB, 0xDD, 0xED, 0xFD,
}
_LEN2 = {
    0x06, 0x0E, 0x16, 0x1E, 0x26, 0x2E, 0x36, 0x3E,
    0xC6, 0xCE, 0xD6, 0xDE, 0xE6, 0xEE, 0xF6, 0xFE,
    0xD3, 0xDB,
}
INSN_LEN = [3 if op in _LEN3 else 2 if op in _LEN2 else 1 for op in range(256)]

# Insertion order is the print order.
REF_KINDS = {
    0x3A: "LDA", 0x32: "STA", 0x2A: "LHLD", 0x22: "SHLD",
    0x01: "LXI B", 0x11: "LXI D", 0x21: "LXI H", 0x31: "LXI SP",
}
KIND_ORDER = list(REF_KINDS.values())


def in_ram(addr):
    return any(lo <= addr <= hi for _, lo, hi in RAM_WINDOWS)


def scan(mem):
    """Return ({target: {kind: [sites]}}, {(dir, port): [sites]})."""
    refs = {}
    ports = {}
    for start, end in CODE_REGIONS:
        a = start
        while a <= end:
            op = mem[a]
            kind = REF_KINDS.get(op)
            if kind:
                target = mem[a + 1] | mem[a + 2] << 8
                if in_ram(target):
                    refs.setdefault(target, {}).setdefault(kind, []).append(a)
            elif op in (0xD3, 0xDB):
                key = ("OUT" if op == 0xD3 else "IN", mem[a + 1])
                ports.setdefault(key, []).append(a)
            a += INSN_LEN[op]
    return refs, ports


def sites(addrs):
    return " ".join(f"{a:04X}" for a in sorted(addrs))


def main():
    mem, owners = load_image()
    refs, ports = scan(mem)

    print("== Code regions scanned")
    for start, end in CODE_REGIONS:
        print(f"{start:04X}-{end:04X} {owner(owners, start)}-{owner(owners, end)}")

    for name, lo, hi in RAM_WINDOWS:
        targets = sorted(t for t in refs if lo <= t <= hi)
        print(f"\n== {name} {lo:04X}-{hi:04X}: {len(targets)} addresses")
        for t in targets:
            kinds = refs[t]
            body = "; ".join(f"{k} {sites(kinds[k])}" for k in KIND_ORDER if k in kinds)
            print(f"{t:04X}  {body}")

    print("\n== I/O ports")
    for d, port in sorted(ports, key=lambda k: (k[1], k[0] != "IN")):
        print(f"{d:3} {port:02X}  {sites(ports[(d, port)])}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
