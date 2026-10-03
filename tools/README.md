# Research tools

Static tools read the ROM images in `roms/224XL_v8_21` directly. Dynamic tools boot the real firmware in the `emu/src` machine. Addresses and bytes are hex throughout. Program IDs are the factory program IDs printed by `lex224emu --list` and listed in `emu/README.md`, for example 01 = CONCERT HALL, 02 = PLATE, 08 = CHAMBER.

|Tool|Kind|Purpose|
|---|---|---|
|`rom_map.py`|static|Checksums, record slots, preset blocks, factory program table, labels (see [docs/rom_map.md](../docs/rom_map.md)). Exits 1 on a checksum or lookup failure. The other Python tools import its `load_image()`|
|`ram_xref.py`|static|Every direct reference to the RAM windows and every `IN`/`OUT`, from a linear sweep of the code regions|
|`ucode_dump.py [ID ...]`|static|Microcode records decoded in DSP execution order, with the record's template offsets|
|`dis8080.py`|static|8080 disassembler, hex dump and operand cross-reference|
|`rom_patch.py`|static|A patched copy of the ROM set, checksums kept valid|
|`lexprobe`|dynamic (C++)|Boot a program, optionally with patched ROM bytes, and report WCS, RAM, access traces or live behaviour|
|`size_model.py`|dynamic|The SIZE offset map of [docs/parameters.md](../docs/parameters.md) §5.2, checked against `lexprobe`|
|`wav_compare.py REF TEST`|audio|Level, band and timing comparison of two renders|

The Python tools run in the repo's uv environment (`pyproject.toml`, `uv.lock`): run `uv sync` once at the repo root to create `.venv` with numpy (`wav_compare.py`) and pyusb (`daisy/tools/cdc_read.py`), then invoke a tool as `uv run tools/NAME.py` from anywhere in the repo. The scripts' shebang is `uv run`, so `tools/NAME.py` works too.

## dis8080.py

```
tools/dis8080.py ADDR COUNT [ADDR COUNT ...]   # COUNT instructions from each ADDR
tools/dis8080.py --hex ADDR COUNT              # COUNT raw bytes
tools/dis8080.py --xref TARGET [TARGET ...]    # CALL/JMP/Jcc/Ccc/LXI/LDA/STA/LHLD/SHLD with that operand
```

For example, `--xref B55B` gives `B55B <- AA2B: CALL B55B`. The cross-reference sweeps `ram_xref.py`'s `CODE_REGIONS` linearly. It therefore shares that sweep's caveats:
- inline data can be read as code;
- indirect targets (`PCHL`, handler tables such as A91E) are not found.

## rom_patch.py

```
tools/rom_patch.py OUTDIR ADDR=VAL [ADDR=VAL ...] [--fix ADDR]
```

This writes all 11 ROM files to OUTDIR with the patches applied. The emulator refuses ROMs with a bad byte sum, so each patched ROM gets one compensation byte to restore its checksum:
- In SBC1 and NVS8, the default is the known checksum-adjust byte: 0003 in SBC1, FFFE in NVS8. The preset blocks at F000 are in NVS8.
- Any other ROM needs `--fix ADDR`, a byte inside that ROM that the firmware does not use. The record-area gaps xFFE are candidates; see [docs/rom_map.md](../docs/rom_map.md) §3.

Use the result with `--rom-dir`, for example to render a preset with DEFINITION (preset byte +10) set to F8:

```
tools/rom_patch.py /tmp/roms_def F010=F8
emu/build/lex224emu --rom-dir /tmp/roms_def "CONCERT HALL" guitar.wav def.wav
```

## lexprobe

`lexprobe` is built with the emulator (`cmake --build emu/build`, see [emu/README.md](../emu/README.md)) as `emu/build/lexprobe`.

```
lexprobe [--rom-dir DIR] COMMAND ID [ADDR=VAL ...] [ARGS]
```

- `ADDR=VAL` patches a ROM byte in memory before the boot (0000–17FF SBC, 8000–FFFF NVS). No checksum fix is needed, because `Machine::boot` does not check checksums.
- Every command boots through the same path as `lex224emu`: entry at 00B4, the program forced at 8163, then 1 s of DSP time.
- "Step n" is the 8080 step number (WCS byte 4000h + 4n). The DSP pc is 127 − n.

|Command|Output|
|---|---|
|`steps ID`|The header `loop L record RRRR`, then one line per step, 127 down to 0 (pc order): `n pc run op template wcs m sign xfer zero prot`. `run` = 1 if the step is inside the loop. `template` is the record's own bytes +0/+1, `wcs` the loaded ones. `m` is the coefficient magnitude, and `xfer`/`zero`/`prot` are 1 when the step asserts them. I/O steps add `src dac wrX reset`|
|`mem ID ADDR[:LEN] ...`|Hex dump of 8080 memory after the boot. LEN is hex, default 10|
|`groups ID`|The step-group table at 3CF4 ([docs/parameters.md](../docs/parameters.md) §2): header, type, count, flags, entry pointer, and the raw entry bytes. For types 0 and 2 it also gives the decoded WCS steps|
|`diff ID BLOCK [DELTA]`|For each byte of the preset block at BLOCK, adds DELTA (decimal, default 8) to it, boots, and lists the changed WCS steps (offset, m, control bits) and changed RAM bytes. Preset blocks: `rom_map.py`, or F000 for CONCERT HALL|
|`handlers ID [PASS]`|Builder pass PASS (default 1, counted at A7A9 entries): every slot-handler entry from table A91E, with the register slot, value, Z flag and group descriptor BC, then the WCS writes it makes. Allpass calls B000/B00D show their inputs|
|`access ID LO[-HI] [SEC]`|Every 8080 read and write of LO..HI, aggregated by phase (boot or audio), address and pc, with the written value during boot. The pc is the address after the instruction's operand bytes. SEC seconds (default 0) of silent audio follow the boot|
|`run ID BURST TOTAL OUT.wav [opts]`|Gaussian noise for BURST s (amplitude `--amp`, default 0.3 of full scale), then silence, TOTAL s in all, at the DSP rate. Writes the 4 DACs to a 4-channel WAV at the rounded DSP rate. Reports the X-register reads by pc, the WCS writes that changed a byte (per step, with the bytes touched) and the options below|

`run` options, each repeatable:

|Option|Effect|
|---|---|
|`--burst2 START,LEN`|A second noise burst, in seconds|
|`--poke ADDR=VAL@SEC`|Write an SBC/NVS RAM byte at SEC seconds into the audio, e.g. `--poke 3CCD=00@1.0` turns the option bits off|
|`--trace STEP`|Print every change of step STEP's m and offset, with its time|
|`--pc ADDR`|Count instruction fetches at ADDR during audio|
|`--watch ADDR`|Print the byte at ADDR every 0.1 s|

Examples:

```
emu/build/lexprobe steps 01 | awk '$4 == "IO"'           # CONCERT HALL's I/O steps
emu/build/lexprobe mem 01 F030=20 3CCE:2 3CD8:13         # SIZE 20: SIZE bytes and map parameters
emu/build/lexprobe run 01 0.5 4 /tmp/r.wav --trace 108   # a MODE ENH tap moving
emu/build/lexprobe access 01 3E44-3E49 2                 # who touches the modulation timers
```

The probe subclasses `Machine` and overrides its bus methods. That is why `Machine`'s internal state is `protected`, not `private`.

## size_model.py

```
tools/size_model.py [--lexprobe PATH] [ID ...]      # default: all 22 factory programs
```

This is a Python transcription of B60C, B6FA and B683 (the stretch factor, the interpolation and the map S). For each program it boots at SIZE 00, 01, 20, 40, 80, C0, FE and FF (RICH SPLIT patches SIZE 2 as well) and classifies every memory-op offset:
- equal to S(template);
- equal to S(base) + d + 1 with d the same at every SIZE. These are the delay steps the builders rewrite;
- other.

For FF records it checks that SIZE moves no offset. It needs `emu/build/lexprobe`. The expected result, one line per program, is that "other" appears only for the MODE ENH taps of RICH CHAMBER, DARK CHAMBER (steps 116/117) and PLATE/CHORUS (step 57). A full run takes about 10 s.

## wav_compare.py

```
tools/wav_compare.py REF.wav TEST.wav [--rms DB] [--band DB]
```

This compares two 16-bit renders of the same length, channel by channel. It checks:
- total RMS, within 0.3 dB by default;
- third-octave bands from 125 Hz to 12.5 kHz from a Welch estimate, within 1.0 dB;
- that the cross-correlation peak over the first 0.5 s after the onset is at lag 0.

Exit status 0 means every channel passed.

The emulated machine is chaotic in its input: the 8080 branches on DSP data it reads through the X register. An emulator change that alters rounding anywhere therefore gives a different waveform even when it is correct. Use this check, not `cmp`, to compare a new build against a reference render from an older build.
