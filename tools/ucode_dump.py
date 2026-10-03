#!/usr/bin/env -S uv run
"""Decode the DSP microcode records of the Lexicon 224XL v8.2 ROM set.

Prints each record's steps in DSP execution order, decoded with the field
layout in docs/dsp_microcode.md. Bytes +0/+1 shown are the record's own; the
loader replaces them at run time with the image it builds at 3E4E-3F4D, so the
delay offsets and I/O bits shown here are the record's template values.

Usage: ucode_dump.py [microcode id (hex) ...]   (default: every record)
"""

import sys

from rom_map import label, load_image, owner, preset_blocks, program_table, record_slots

STEP_AREA = 0xAA          # record offset of step 0
STEPS = 128
MEM_OPS = {3: "--", 2: "IO", 1: "MW", 0: "MR"}               # CPU word bits 17,16
READS = {3: None, 2: "RDRREG", 1: "RDXREG", 0: "RDAD"}        # CPU word bits 13,12
DAC_CHANNELS = (("A", 11), ("B", 10), ("C", 9), ("D", 8))    # WR DA select bits
IO_FLAGS = (("WRXREG", 6), ("TEST", 5), ("DP", 4), ("RESET", 3))


def step_words(mem, addr):
    """The record's 128 steps as little-endian CPU words (byte +0 = bits 0-7)."""
    base = addr + STEP_AREA
    return [int.from_bytes(mem[base + 4 * n:base + 4 * n + 4], "little") for n in range(STEPS)]


def bit_clear(word, bit):
    return not word >> bit & 1


def fields(word):
    """Decode one CPU-view step word. Active-low control bits are returned as booleans."""
    low = word & 0xFFFF
    f = {
        "op": MEM_OPS[word >> 16 & 3],
        "low": low,
        "wa": word >> 18 & 3,
        "ra": word >> 20 & 3,
        "slot": bool(word >> 22 & 1),
        "negative": bit_clear(word, 23),
        "xfer": bit_clear(word, 24),
        "zero": bit_clear(word, 25),
        "coef": ~word >> 26 & 0x3F,
    }
    if f["op"] == "IO":
        f["read"] = READS[low >> 12 & 3]
        f["dac"] = "".join(ch for ch, bit in DAC_CHANNELS if bit_clear(low, bit)) if bit_clear(low, 7) else ""
        f["flags"] = [name for name, bit in IO_FLAGS if bit_clear(low, bit)]
    return f


def describe(word):
    f = fields(word)
    if f["op"] in ("MR", "MW"):
        action = f"{f['op']} ofs={f['low']}"
    elif f["op"] == "IO":
        io = [f["read"]] if f["read"] else []
        if f["dac"]:
            io.append(f"WRDA:{f['dac']}")
        io += f["flags"]
        action = "IO " + " ".join(io)
    else:
        action = "--"
    tokens = [f"{action:28}", f"w{f['wa']}", f"r{f['ra']}"]
    if f["coef"]:
        tokens.append(f"{'-' if f['negative'] else '+'}{f['coef'] / 32:g}")
    tokens += [name.upper() for name in ("xfer", "zero", "slot") if f[name]]
    return " ".join(tokens)


def execution_order(words):
    """(pc -> CPU step list, RESET pc or None). WCS address = 127 - CPU step; the
    step after RESET still executes (delay slot) before the counter restarts."""
    order = [STEPS - 1 - pc for pc in range(STEPS)]
    for pc, n in enumerate(order):
        f = fields(words[n])
        if f["op"] == "IO" and "RESET" in f["flags"]:
            return order[:pc + 2], pc
    return order, None


def allpass_matches(words, order):
    """(matches, candidates) for MR +-g, MW k, MW -+g runs where k = 32(1 - g^2)."""
    seq = [fields(words[n]) for n in order]
    total = hits = 0
    for a, b, c in zip(seq, seq[1:], seq[2:]):
        if (a["op"], b["op"], c["op"]) == ("MR", "MW", "MW") and a["coef"] and \
                a["coef"] == c["coef"] and a["negative"] != c["negative"]:
            total += 1
            g = a["coef"] / 32
            hits += abs(b["coef"] - round(32 * (1 - g * g))) <= 1
    return hits, total


def program_names(mem):
    """{microcode id: [factory program names]}."""
    micro = {}
    for _, pid, _, mid in preset_blocks(mem):
        micro.setdefault(pid, mid)
    names = {}
    for pid, _, _, lbl, _ in program_table(mem):
        if pid in micro:
            names.setdefault(micro[pid], []).append(label(mem, 0, lbl))
    return names


def dump(mem, owners, slot, addr, mid, names):
    words = step_words(mem, addr)
    order, reset_pc = execution_order(words)
    seq = [(pc, n, fields(words[n])) for pc, n in enumerate(order)]
    title = ", ".join(names.get(mid, [])) or "no factory program"
    print(f"== Record #{slot} @{addr:04X} {owner(owners, addr)} id={mid:02X} ({title})")
    if reset_pc is None:
        print("no RESET step: loop length unknown, all 128 steps shown")
    else:
        print(f"loop: {len(order)} steps (RESET at pc {reset_pc}, delay slot pc {reset_pc + 1})")
    reads = [pc for pc, _, f in seq if f["op"] == "IO" and f["read"] == "RDAD"]
    outs = {ch: [pc for pc, _, f in seq if f["op"] == "IO" and ch in f["dac"]] for ch, _ in DAC_CHANNELS}
    print("ADC reads at pc " + " ".join(map(str, reads)) + "; DAC writes " +
          " ".join(f"{ch}@{','.join(map(str, pcs))}" for ch, pcs in outs.items() if pcs))
    print("SBC access slots at pc " + (" ".join(str(pc) for pc, _, f in seq if f["slot"]) or "none"))
    hits, total = allpass_matches(words, order)
    print(f"allpass k=32(1-g^2) matches: {hits}/{total}")
    executed = set(order)
    skipped = [n for n in range(STEPS) if n not in executed and words[n] != 0xFFFFFFFF]
    print(f"not executed: {STEPS - len(order)} steps, {len(skipped)} of them not FF-filled")
    print(" pc step  +0 +1 +2 +3  decoded")
    for pc, n, _ in seq:
        raw = " ".join(f"{b:02X}" for b in words[n].to_bytes(4, "little"))
        print(f"{pc:3} {n:4}  {raw}  {describe(words[n])}")
    print()


def main(argv):
    mem, owners = load_image()
    records = [(slot, addr, mid) for slot, addr, mid in record_slots(mem) if mid != 0xFF]
    try:
        wanted = {int(a, 16) for a in argv}
    except ValueError:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    unknown = wanted - {mid for _, _, mid in records}
    if unknown:
        known = " ".join(f"{mid:02X}" for _, _, mid in records)
        print(f"unknown microcode id(s): {' '.join(f'{m:02X}' for m in sorted(unknown))}; known: {known}",
              file=sys.stderr)
        return 2
    names = program_names(mem)
    for slot, addr, mid in records:
        if not wanted or mid in wanted:
            dump(mem, owners, slot, addr, mid, names)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
