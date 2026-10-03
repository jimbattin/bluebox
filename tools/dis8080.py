#!/usr/bin/env -S uv run
"""8080 disassembler and reference finder for the Lexicon 224XL v8.2 ROM set.

Usage:
  dis8080.py ADDR COUNT [ADDR COUNT ...]   disassemble COUNT instructions from ADDR
  dis8080.py --hex ADDR COUNT              hex dump of COUNT bytes
  dis8080.py --xref TARGET [TARGET ...]    instructions whose 16-bit operand is TARGET
                                           (CALL/JMP/Jcc/Ccc/LXI/LDA/STA/LHLD/SHLD)

Addresses are hex. --xref sweeps the code regions of ram_xref.py linearly, so
it shares that sweep's false-hit caveat (inline data read as code).
"""

import sys

from rom_map import load_image
from ram_xref import CODE_REGIONS

R = ["B", "C", "D", "E", "H", "L", "M", "A"]
RP = ["B", "D", "H", "SP"]
RPP = ["B", "D", "H", "PSW"]
CC = ["NZ", "Z", "NC", "C", "PO", "PE", "P", "M"]
ALU = ["ADD", "ADC", "SUB", "SBB", "ANA", "XRA", "ORA", "CMP"]
ALUI = ["ADI", "ACI", "SUI", "SBI", "ANI", "XRI", "ORI", "CPI"]
ROT = ["RLC", "RRC", "RAL", "RAR", "DAA", "CMA", "STC", "CMC"]


def dis1(mem, a):
    """Return (length, text, 16-bit operand or None) of the instruction at a."""
    o = mem[a]
    b1 = mem[(a + 1) & 0xFFFF]
    w = b1 | mem[(a + 2) & 0xFFFF] << 8
    x, y, z = o >> 6, (o >> 3) & 7, o & 7
    if o == 0x76:
        return 1, "HLT", None
    if x == 1:
        return 1, f"MOV {R[y]},{R[z]}", None
    if x == 2:
        return 1, f"{ALU[y]} {R[z]}", None
    if x == 0:
        if z == 0:
            return 1, "NOP", None
        if z == 1:
            return (3, f"LXI {RP[y >> 1]},{w:04X}", w) if y % 2 == 0 else (1, f"DAD {RP[y >> 1]}", None)
        if z == 2:
            if y < 4:
                return 1, ["STAX B", "LDAX B", "STAX D", "LDAX D"][y], None
            return 3, f"{['SHLD', 'LHLD', 'STA', 'LDA'][y - 4]} {w:04X}", w
        if z == 3:
            return 1, f"{'INX' if y % 2 == 0 else 'DCX'} {RP[y >> 1]}", None
        if z == 4:
            return 1, f"INR {R[y]}", None
        if z == 5:
            return 1, f"DCR {R[y]}", None
        if z == 6:
            return 2, f"MVI {R[y]},{b1:02X}", None
        return 1, ROT[y], None
    if z == 0:
        return 1, f"R{CC[y]}", None
    if z == 1:
        if y % 2 == 0:
            return 1, f"POP {RPP[y >> 1]}", None
        return 1, ["RET", "RET", "PCHL", "SPHL"][y >> 1], None
    if z == 2:
        return 3, f"J{CC[y]} {w:04X}", w
    if z == 3:
        if y <= 1:
            return 3, f"JMP {w:04X}", w
        if y == 2:
            return 2, f"OUT {b1:02X}", None
        if y == 3:
            return 2, f"IN {b1:02X}", None
        return 1, ["XTHL", "XCHG", "DI", "EI"][y - 4], None
    if z == 4:
        return 3, f"C{CC[y]} {w:04X}", w
    if z == 5:
        if y % 2 == 0:
            return 1, f"PUSH {RPP[y >> 1]}", None
        return 3, f"CALL {w:04X}", w
    if z == 6:
        return 2, f"{ALUI[y]} {b1:02X}", None
    return 1, f"RST {y}", None


def disassemble(mem, a, n):
    for _ in range(n):
        length, text, _ = dis1(mem, a)
        raw = " ".join(f"{mem[(a + i) & 0xFFFF]:02X}" for i in range(length))
        print(f"{a:04X}: {raw:9} {text}")
        a = (a + length) & 0xFFFF


def xrefs(mem, targets):
    hits = []
    for start, end in CODE_REGIONS:
        a = start
        while a <= end:
            length, text, w = dis1(mem, a)
            if w in targets:
                hits.append((w, a, text))
            a += length
    return sorted(hits)


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        sys.exit(__doc__)
    mem, _ = load_image()
    try:
        if argv[0] == "--hex" and len(argv) == 3:
            a, n = int(argv[1], 16), int(argv[2])
            print(" ".join(f"{mem[(a + i) & 0xFFFF]:02X}" for i in range(n)))
        elif argv[0] == "--xref" and len(argv) > 1:
            for w, a, text in xrefs(mem, {int(t, 16) for t in argv[1:]}):
                print(f"{w:04X} <- {a:04X}: {text}")
        elif len(argv) % 2 == 0 and not argv[0].startswith("-"):
            for i in range(0, len(argv), 2):
                disassemble(mem, int(argv[i], 16), int(argv[i + 1]))
                print()
        else:
            sys.exit(__doc__)
    except ValueError:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
