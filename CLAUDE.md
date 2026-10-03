This project reverse-engineers the Lexicon 224XL firmware (ROM set **224XL V8.2**), emulates it (`emu/`), and runs the emulation as a guitar pedal (`daisy/`).

Conventions here and in `docs/`: `[INFERENCE]` = derived from data or behaviour, not traced in a schematic; `[unknown]` = not yet determined.

# Where things are
- `docs/rom_map.md` (ROM contents, program/record tables, boot flow), `docs/memory_map.md` (address space, RAM variables, I/O ports), `docs/dsp_microcode.md` (WCS format, execution model), `docs/aru_fpc.md` (ARU and FPC arithmetic bit by bit, ROM self-test vectors), `docs/parameters.md` (sliders, options, builders, live behaviour). Each has an "Open questions" section.
- `tools/` (see `tools/README.md`): ROM tables, RAM/port xref, microcode decoder, 8080 disassembler, ROM patcher, SIZE model, render comparison, and `lexprobe` (boots a program and dumps/traces WCS, RAM, builders and accesses).
- `emu/`: the `lex224emu` CLI and the allocation-free engine `emu/src` shared with the pedal (see `emu/README.md`).
- `daisy/`: Terrarium pedal firmware for the Daisy Seed (see `daisy/README.md`); `daisy/bench/` is an on-device engine benchmark.
- `editor/`: the Bluebox BARC Web MIDI editor for the pedal (TypeScript, Preact, Bun), documented in `daisy/README.md` "Editor and USB log".
- `roms/224XL_v8_21/`: `SBC1–3 2716.BIN`, `NVS1–8 2732.BIN` (the ROM itself reads `224XL V8.2`).
- Manuals: `224_service_manual.pdf` (original Model 224, image-only scan; cite 1-based PDF pages); [224X service manual](https://therackofages.com/Lexicon/224x%20digital%20reverborator/Lexicon-224X-Service-Manual.pdf), the authority for the DSP cards (pp.17–29 theory, p.90 ARU, pp.92–93 FPC, pp.110–111 T&C); [Intel SBC 80/10 HRM](http://www.bitsavers.org/pdf/intel/iSBC/9800230F_iSBC_80-10_SBC_Hardware_Reference_Manual_Aug77.pdf).

# Build, test, flash
```
uv sync                                  # once: .venv for the Python tools (numpy, pyusb); run them with `uv run`
cmake -S emu -B emu/build && cmake --build emu/build && ctest --test-dir emu/build --output-on-failure
export PATH=$HOME/.arm/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi/bin:$PATH
git submodule update --init --recursive && make -C daisy/libDaisy   # once: libDaisy v8.1.0 submodule
make -C daisy test                       # host tests of daisy/src/pedal_controls.h and editor_protocol.h
make -C daisy && make -C daisy program-dfu   # libDaisy: daisy/libDaisy submodule
make -C daisy EMBED_ROMS=0 && make -C daisy EMBED_ROMS=0 program-dfu   # no ROM images (build_noroms/); flash them from the editor
make -C editor serve                     # editor at http://localhost:3000; `html` -> dist/editor.html; `test check fmt`
uv run daisy/tools/cdc_read.py [SECONDS]     # USB log (LOG SysEx; close the editor first); needs libusb
```
- DFU is entered by hand: hold BOOT, tap RESET, release BOOT.
- Newlib `_close`/`_read`/… stub and RWX-segment linker warnings are expected.
- The emulator is chaotic in its input: compare renders with `tools/wav_compare.py`, not byte for byte. Pedal and host renders differ anyway: the ARM build fuses float multiply-adds.
- No undefined behaviour affects the output (audit and sanitizer recipe in `emu/README.md` "Undefined behaviour"). The ARU's bit-exactness relies on arithmetic right shift and wrapping narrowing casts, which are implementation-defined in C++17 (GCC and Clang provide both).

# Pedal (`daisy/`)
- Mono in, mono out: DAC A, or the mean of A and C for eight of the nine A=D/B=C programs, or the mean of all four DACs for PLATE/CHORUS (a user choice). CD PLATE A uses A alone, because its A and C comb-filter when averaged; the editor can override this per program (DEFAULT, A, C, A+C, A+B+C+D), saved in QSPI (`daisy/README.md` "Mono output"). Controls: FS1 bypass; FS2 tap = next program in the bank, FS2 hold = secondary knob layer; knobs drive dry/wet and LARC sliders live; switch 1 MODE ENH, switch 2 DECAY OPT, switches 3/4 one of 4 banks (6/6/6/4). Details in `daisy/README.md`.
- The Seed's USB port is a USB MIDI device (libDaisy runs one class per port, so no CDC): the BARC editor selects any program and moves every slider page, options (incl. DYN DECAY), mix, bypass and the per-program mono output through SysEx (`daisy/src/editor_protocol.h`), and the log travels as LOG SysEx. Last mover wins: levers and the bank selector act only on their own changes. The link is checked on the device from the host (`aseqsend`/`aseqdump`; CONCERT HALL 707 / 730 permille with it running), and the browser editor's link, ROM panel and program select from headless Chromium (localhost, with the `midi`/`midi-sysex` permissions granted). With no USB host the pedal runs normally and drops its log.
- ROMs: the engine runs from a RAM copy of the 11 ROMs (AXI SRAM). The editor's ROM panel flashes each one (ROM_BEGIN/DATA/COMMIT, CRC-32 and socket byte sum checked) into its own 4 KiB QSPI sector at chip offset 0x7F0000 + slot × 0x1000; a valid flashed image overrides that slot's built-in one, ERASE/REVERT brings it back. `EMBED_ROMS=0` builds carry no images: with a slot empty the pedal stays in NoRoms (dry only, LED2 at 1 Hz). Checked on the device: flash, erase, revert, and the store surviving a power cycle and `program-dfu` (`daisy/README.md` "ROMs"). The sector after the ROM slots (0x7FB000) holds the mono-output overrides (`OutMixTable`).
- MPU (`daisy/src/mpu.h`, pedal and bench): ST's no-access background region over 0x60000000–0xDFFFFFFF keeps speculative reads off the stalling QSPI byte 0x907FFFFF (ES0392 2.7.5), plus a read-only window for the ROM and mono-output store. Crash recovery (`daisy/src/crash_trace.h`) turns a CPU fault, an audio stall (100 ms) or a hang (4 s watchdog) into a restart with an `event` log line (`daisy/README.md` "Lockups").
- Control logic lives only in `daisy/src/pedal_controls.h` (host-tested); the emulator exposes only `Machine::moveSlider`/`setOption`/`setBootOptions`, which inject calls into the firmware's own handlers, and the read-backs `sliderValues`/`options`; `Engine::setOutMix` changes the pedal's DAC mix live (not a firmware control).
- The audio callback touches the engine only while the state is Running; the main loop owns it otherwise (program loads re-boot the whole machine, 0.77–0.82 s, wet muted).
- Dry/wet is equal-power, with a per-program wet trim after the engine (`kWetTrim`) that matches wet loudness to dry. Open issue: the knob's taper is linear in angle, not perceptual (`daisy/README.md` "Open questions").
- No heap anywhere (`nm -u` check in `daisy/README.md`). DMEM sits in AXI SRAM (`daisy/STM32H750IB_sram_lex224.lds`).
- Device load at 480 MHz, with the 8080 wait states (2026-10-03): engine bench 653–678 permille average, 688–717 max over all 22 programs, no underruns; pedal CONCERT HALL 671–673 / 708–731 (built-in or flashed ROM images). Before the wait states: bench 690–716 / 706–734, pedal 710 / 727; before the DSP-loop rewrite the pedal measured 769–793 / 781–817 with knob sweeps and FS2 changes. A MODE ENH flip overruns one callback (3.5–3.9 ms). Latency 184–201 samples at 48 kHz.

# The 224XL, as understood
Details and evidence are in `docs/`; this is the working summary.

**Hardware.** An 8080 SBC (Intel SBC 80/10A or National BLC-11) runs the UI, the LARC link (8251 USART, ports EE/EF; protocol `[unknown]`) and program loading. The DSP is discrete 74-series logic on three cards: ARU (multiply-accumulate), DMEM (delay memory) and T&C (timing and the 128-step writable control store). Converters are 12-bit (DAC80) with 2 gain bits (4 ranges, 0/6/12/18 dB) to 16-bit fixed point via the FPC: the word is the 12 bits shifted left 4..1 times, so the step is 16..2 (`docs/aru_fpc.md` §4); the emulator models both directions as `fpcQuantize()`, with power-of-two gain thresholds standing in for the AIN's analog comparators. The AIN card pre-emphasises ahead of the ADC (shelf, 50 µs / 12.5 µs, 224X manual §3.9) and AOUT de-emphasises; `Engine` applies both at the host rate (`emu/src/emphasis.h`, inverse on output `[INFERENCE]`).

**Timing.** Master clock 30.72 MHz ÷ 9 = one DSP step (293 ns). One sample = one pass of the microprogram, so the rate is 30.72 MHz / 9 / loop length: 100–109 steps in V8.2, 34133–31315 Hz. The 8080 runs at 2.048 MHz from the SBC's own 18.432 MHz crystal (15 master ticks per T-state); the emulator puts both on one timeline and charges each off-board (Multibus) machine cycle of the 8080 one wait state (`Machine::kWaitStates`, `[INFERENCE]`; `docs/dsp_microcode.md`).

**8080 map.** 0000–17FF SBC ROMs 1–3; 2000–27FF NVS battery RAM; 3C00–3FFF SBC RAM (stack from 4000h down); 4000–41FF WCS (128 steps × 4 bytes); 8000–FFFF NVS EPROMs 1–8. Each ROM's byte sum equals its socket index. DSP control is ports 00–09 (`docs/memory_map.md`); the X registers (ports 06/07) are the only data path between the 8080 and the DSP: OUT writes the one the DSP reads (`RD XREG`), IN reads the one the DSP writes (`WR XREG`). Ports 08/09 are the peak-hold headroom registers that feed the firmware's level detector (`fpcHeadroom()`, bit order `[INFERENCE]`).

**WCS microinstructions** (`docs/dsp_microcode.md`):
- Every bit is the complement of what the 8080 wrote (FF-filled = no-op). Steps are little-endian words at 4000h + 4n; 8080 step n is WCS address 127 − n, and execution starts at step 127 (pc 0) and runs down.
- Fields: bits 0–15 DMEM offset or I/O sub-op; 16–17 bus op; 18–19 / 20–21 register write/read address; 22 `PROT` (SBC access slot); 23 coefficient sign (1 = positive, fixed by the ROM's ARU self-test); 24 `XFER`; 25 `ZERO`; 26–31 coefficient magnitude m (`~bits & 3F`), gain m/32.
- `RESET` at pc p ends the loop after a no-op delay slot (loop = p + 2). The loader writes each step's offset half from the firmware-built image at 3E4E–3F4D, which fixes every program's loop length.
- The 8080 may touch the WCS while the DSP runs only in the two `PROT` steps per program; the access stalls until one comes round. The firmware rewrites coefficients live (delay modulation); the stalls take about 4% of the 8080's time, so the 8080's own speed, wait states included, sets the modulation rate.

**ARU and DMEM.** 4 × 16-bit register file, 16 × 6-bit multiplier, 20-bit saturating accumulator, one MAC per step, result pipelined one step. The emulator's MAC is bit-exact to the ROM's own ARU self-test vectors (`docs/aru_fpc.md`, `emu/tests/aru_test.cpp`): the data bus carries the complement of the 8080's value `[INFERENCE]`, so in 8080 terms the accumulator is in 1/8 LSB, `ZERO` sets it to −1, each set bit b of m adds ±(((8x + 7) >> (5 − b)) + 1), it clamps to 19 bits, and `XFER` takes acc >> 3. DMEM address = position counter − offset. V8.2 requires 64 K words (stock XL units have 32 K): the DMEM test always covers 64 K and 14 of 22 programs use offsets above 32 K.

**Firmware.** Reset → 003B (stack, 8251) → LARC handshake and self-tests → 00CC: a non-zero byte at B800h jumps to the NVS1 application at 813B. A program is table entry A446 → preset block (F000 chain) → microcode record (21 slots of 2AAh bytes from B800h: 170-byte header + 128 steps). The loader (NVS3 AA00–AB6E) builds the offset image, halts the DSP, copies the steps and restarts it. NVS1 8163 is `CALL 13B6h` with A = program ID; the emulator enters at the warm restart 00B4 (no self-tests, no LARC) and forces the program there.

**Parameters** (`docs/parameters.md`). Preset bytes +05…+33 hold slider and option values, copied to the program register 3CA2–3CD1; option bits are 3CCD (01 DYN DECAY, 40 MODE ENH, 80 DECAY OPT), SIZE is 3CCE. Each record lists the DSP steps each parameter drives; the rebuild A7A9 walks the handler table A91E and recomputes changed slots, and A791 is the full reprogram. The firmware never polls the register: live changes go through those rebuilds (the LARC slider handler is NVS1 8614).

# History
- **224** (1978): 20 kHz sample rate, ~8 kHz bandwidth, firmware up to 4.3. The **NVS card** (firmware 4.4) added battery RAM and more EPROM for user and factory programs.
- **224X**: new DMEM and T&C cards, 16 kHz bandwidth; last firmware 8.1.
- **224XL**: 224X plus the LARC remote (alphanumeric displays, tape patch storage; "LURCH" in the schematics).
