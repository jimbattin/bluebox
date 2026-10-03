# lex224emu

`lex224emu` is a command-line emulator of the Lexicon 224XL. It runs the real v8.2 firmware (`roms/224XL_v8_21`) on an emulated 8080 SBC. The firmware loads a factory program into the DSP's writable control store, and the emulator runs that microcode on the audio from a WAV file.

The output is 100% wet, as on the hardware. The factory programs have no dry path.

## Build

Requirements: CMake ≥ 3.16 and a C++17 compiler (GCC or Clang). Ninja is optional.

From the repository root:

```
cmake -S emu -B emu/build -G Ninja
cmake --build emu/build
```

Without Ninja, leave out `-G Ninja`. The build type defaults to `Release`. It produces:
- `emu/build/lex224emu`, the emulator described here;
- `emu/build/lexprobe`, the research probe (see [tools/README.md](../tools/README.md));
- five test programs, run with `ctest` (see [Tests](#tests)).

All of them link the static library `lexcore`, which holds everything in `src/` except `main.cpp`.

Builds must be out of source. `cmake -S emu -B emu` stops with an error.

The ROM directory is compiled in as the default, so the binary finds the ROMs wherever you run it from. Use `--rom-dir` to point it somewhere else.

### Tests

```
ctest --test-dir emu/build --output-on-failure
```

The whole run takes a few seconds in Release. There are five tests:
- **`resampler`** (`tests/resampler_test.cpp`) checks the streaming resampler against fixed tolerances, at CONCERT HALL's DSP rate D = 30.72 MHz / 9 / 105. It uses no ROMs and no files.
  - A 48 kHz → D → 48 kHz round trip of 0.5-amplitude sines stays within ±0.1 dB at 1, 5, 10 and 14.5 kHz, and within ±1 dB at 15 kHz.
  - A 20 kHz tone resampled from 48 kHz to D is at least 75 dB down.
  - A 10 kHz tone resampled from D to 48 kHz leaves an image at D − 10 kHz at least 75 dB down.
- **`engine`** (`tests/engine_test.cpp`) checks the engine's mechanics on a seeded noise-burst stimulus, bit for bit:
  - CONCERT HALL gives the same output in blocks of 48, 1 and 64 samples, with no underruns;
  - `Engine<2,4>` with one output matches `Engine<1,1>`;
  - starting PLATE, running it for 2 s and restarting CONCERT HALL reproduces a fresh CONCERT HALL run;
  - there are no underruns at 8, 44.1, 96 and 192 kHz for CONCERT HALL, PLATE and RICH PLATE, with blocks of 1 and 64;
  - on CHORUS&ECHO, `outMix` 0x05 equals the mean of separate DAC A and DAC C outputs within 1e-5;
  - switching that engine to `outMix` 0x04 with `setOutMix` while it runs gives the separate DAC C output within 1e-5, and `setOutMix` rejects mix 0, 16 and a missing output.

  It loads the ROMs from the compiled-in ROM directory.
- **`slider`** (`tests/slider_test.cpp`) checks the live LARC controls (see [Live controls](#live-controls)) against register values measured on the real ROMs:
  - on CONCERT HALL, slider moves land in the program's register slots with the LARC's clamps (LF DECAY FF → F9, CROSSOVER 00 → 08, DEFINITION FF → C0), and MID DECAY C0 rebuilds the step-44 coefficient;
  - a second move of the same slider while the first is queued replaces it;
  - HALL/HALL page 2 and page 3, and CHORUS&ECHO page 3 (no page 1), use the right storage index;
  - MODE ENH off freezes the modulated step 108 and on restarts it, DECAY OPT clears bit 80, and a bad option mask is refused;
  - MODE ENH forced off at boot freezes step 108 from the start;
  - through `Engine<1,1>`, a noise burst's tail 1–2 s later is at least 10 dB louder with MID DECAY C0 than with 20.
- **`aru`** (`tests/aru_test.cpp`) replays two of the ROM's DSP self-tests on `Dsp` alone, reading programs, inputs and expected words from the ROM image, and names every mismatching vector (see [docs/aru_fpc.md](../docs/aru_fpc.md) §3):
  - the multiplier test SBC2 0942: 20 multiply results at ±21/32, ±42/32 and +63/32, including saturation;
  - the register-file test SBC2 0C48: 8 register read-backs, which also need the separate X-in and X-out registers.
- **`selftest`** (`tests/selftest_test.cpp`) runs the ROM's whole power-on self-test sequence (SBC1 0703 to 00CC) as firmware on `Machine`, via `Machine::runFrom`. It records every error the ROM reports (its error display at 0217/021D is replaced by a `RET`) and prints each test as `pass` or `FAIL`:
  - ROM checksums, NVS RAM, SBC RAM, WCS RAM, ARU register file (0C48), ARU multiplier (0942) and the 64 K DMEM test (0B75) pass;
  - the DSP bus and single-cycle test (0A1D) is listed check by check. 5 of its 20 checks pass: single-cycle passes, the separate X registers, continuous run, and one read that passes only on a stale X value. The other 15 fail as expected: they read the diagnostic ports IN 00–05 (microinstruction fields, bus test register, ARU state) or need the halted DSP to decode its parked step, which the emulator does not model.

  The test fails when any result differs from its expectation, including a check that starts passing. Single-cycle mode is modelled for it: after `OUT 00`, each `OUT 03` runs one pass of the microprogram `[INFERENCE]`.

These tests do not compare against stored reference audio, because the emulated machine is chaotic in its input (see [tools/README.md](../tools/README.md), `wav_compare.py`). To compare an emulator change against an older build, render the same input with both binaries and use `tools/wav_compare.py`.

### Clean build

To recompile every object but keep the configuration (generator, build type, compiler):

```
cmake --build emu/build --clean-first
```

For a full clean build, delete the build directory and configure again:

```
rm -rf emu/build
cmake -S emu -B emu/build -G Ninja
cmake --build emu/build
```

Use the full clean after any of these:
- changing the generator (CMake refuses to switch generators in an existing build directory);
- changing the compiler;
- moving or renaming the repository. The build directory records absolute paths, including the ROM directory that is compiled in.

With CMake ≥ 3.24, `cmake --fresh -S emu -B emu/build -G Ninja` discards the cached configuration without deleting the directory.

## Usage

```
lex224emu [--rom-dir DIR] [--quad] [--duration SEC] [--tail SEC] PRESET IN.wav OUT.wav
lex224emu [--rom-dir DIR] --list
```

|Option|Default|Meaning|
|---|---|---|
|`--rom-dir DIR`|`roms/224XL_v8_21`|Directory holding `SBC1–3 2716.BIN` and `NVS1–8 2732.BIN`. The emulator checks each ROM's size and checksum|
|`--quad`|off|Write four channels (DAC A, B, C, D) instead of stereo (L = DAC A, R = DAC C)|
|`--duration SEC`|whole file|Process only the first `SEC` seconds of the input|
|`--tail SEC`|3.0|Seconds of silence appended so the reverb tail can decay|
|`--list`||Print the factory preset names and exit|

`PRESET` matching is case-insensitive. Quote names that contain spaces or `&`.

**Input.** RIFF/WAVE files at 8000–192000 Hz are accepted in these formats:
- 16-, 24- or 32-bit PCM;
- 32-bit float;
- WAVE_FORMAT_EXTENSIBLE containing either of the above.

For channels:
- mono input feeds both DSP inputs;
- with two or more channels, channel 0 is L and channel 1 is R, and any others are ignored.

**Output.** 16-bit PCM at the input sample rate, `input length + tail` long.

Internally, the audio is resampled to the program's DSP rate (30.72 MHz / 9 / loop length, about 31–34 kHz) and back, in streaming blocks of 48 frames, by the same engine the Daisy pedal firmware (`daisy/`) runs. The resampler is a Kaiser-windowed sinc. It is flat to 0.46 × the DSP rate (about 14.4–15.7 kHz) and 80 dB down at the DSP Nyquist, so it acts as the anti-alias filter on the way in and the anti-image filter on the way out. The engine's fixed latency (194 samples for CONCERT HALL at 48 kHz) is removed from the output file.

The engine also models the 224X converter emphasis, always on (`src/emphasis.h`). Before the input resampler, a shelving pre-emphasis H(s) = (1 + s·50 µs)/(1 + s·12.5 µs) is applied at the host rate. This is the AIN card's network (224X service manual §3.9): +1.3 dB at 2 kHz, +7.2 dB at 8 kHz, +12 dB asymptote. After the output resampler, its exact inverse is applied. The AOUT de-emphasis stages (§3.10, U13/U21) have no published values, so the inverse is `[INFERENCE]`. The net linear response is flat. The ADC clamp, the gain ranging, the headroom ports and the level detector all see the emphasised input, and DSP noise and modulation products are de-emphasised on the way out. Against a build without emphasis, on guitar at −12 dBFS, 21 of the 22 programs agree within `tools/wav_compare.py`'s tolerances. CHORUS&ECHO loses 1.5 dB at 10 kHz and 3.9 dB at 12.5 kHz, because most of its HF output there is produced inside the DSP rather than passed through from the input.

**Status and errors.** A status line goes to stderr, for example:

```
program CONCERT HALL (id 01) record B800, loop 105 steps, DSP rate 32507.94 Hz, boot 3.58 s, WCS writes during audio 50629
```

`boot` is emulated 8080 time spent until the program is loaded and running. `WCS writes during audio` counts the firmware's live coefficient updates (delay modulation).

Exit codes:
- 0: success;
- 1: I/O, ROM, sample-rate or boot error, or the DSP halted during processing;
- 2: bad arguments or unknown preset. For an unknown preset, the list of available presets is printed.

## Examples

List the presets:

```
emu/build/lex224emu --list
```

Process a whole file with CONCERT HALL. This writes a stereo file with a 3 s tail:

```
emu/build/lex224emu "CONCERT HALL" guitar.wav hall.wav
```

Process the first 10 seconds with a 5 s tail:

```
emu/build/lex224emu --duration 10 --tail 5 "concert hall" guitar.wav hall10.wav
```

Write all four DAC outputs:

```
emu/build/lex224emu --quad --duration 10 PLATE guitar.wav plate_quad.wav
```

Preset names containing `&` must be quoted:

```
emu/build/lex224emu --duration 10 "CHORUS&ECHO" guitar.wav chorus.wav
```

Use a different ROM set:

```
emu/build/lex224emu --rom-dir /path/to/roms --list
```

## Presets

The 22 factory programs of the v8.2 ROM set are listed below in ROM-table order, as printed by `--list`.

|Preset|Program id|Loop steps|DSP rate (Hz)|`--quad` outputs|
|---|---|---|---|---|
|CONCERT HALL|01|105|32507.94|4 distinct|
|BRIGHT HALL|20|105|32507.94|4 distinct|
|DARK HALL|05|107|31900.31|4 distinct|
|ROOM|04|105|32507.94|4 distinct|
|SMALL ROOM|40|105|32507.94|A=D, B=C|
|CHAMBER|08|100|34133.33|4 distinct|
|RICH CHAMBER|80|108|31604.94|4 distinct|
|PLATE|02|100|34133.33|A=D, B=C|
|SMALL PLATE|03|100|34133.33|A=D, B=C|
|CD PLATE A|21|100|34133.33|A=D, B=C|
|CD PLATE B|10|102|33464.05|A=D, B=C|
|CHORUS&ECHO|0C|100|34133.33|A=D, B=C|
|RES CHORDS|14|100|34133.33|A=D, B=C|
|M BAND DELAY|24|100|34133.33|A=D, B=C|
|HALL/HALL|06|102|33464.05|4 distinct|
|PLATE/PLATE|0A|104|32820.51|4 distinct|
|PLATE/HALL|12|104|32820.51|4 distinct|
|PLATE/CHORUS|22|105|32507.94|4 distinct|
|RICH PLATE|11|109|31314.98|A=D, B=C|
|DARK CHAMBER|41|108|31604.94|4 distinct|
|INVERSE ROOM|81|108|31604.94|4 distinct|
|RICH SPLIT|42|104|32820.51|4 distinct|

"A=D, B=C" means the program writes DAC A and DAC D together, and DAC B and DAC C together. For those presets, `--quad` output contains only two distinct signals, each appearing twice.

The stereo default (L = DAC A, R = DAC C) carries both distinct signals of every A=D/B=C preset. For a mono output, the Daisy pedal averages A and C for eight of those nine presets, all four DACs for PLATE/CHORUS (a user choice), and uses DAC A alone for the rest (the pedal's editor can override this per program). CD PLATE A is the exception: its C is largely its A, inverted and 11.7 ms later, so averaging them comb-filters. The original 224 manual's mono table and these measurements are in [daisy/README.md](../daisy/README.md#mono-output); V8.2 has no output table, so the choice is `[INFERENCE]`. The engine's `EngineConfig::outMix[k]` is the bitmask of DACs (A = 1, B = 2, C = 4, D = 8) averaged into output *k*.

## Live controls

`Machine::moveSlider(page, slider, position)` moves a LARC slider and `Machine::setOption(mask, on)` toggles DYN DECAY (01), MODE ENH (40) or DECAY OPT (80) while a program runs. `Engine` forwards both, plus `hasPage`. Requests are queued (32 entries; a queued request for the same slider or option is replaced) and injected one at a time as calls from the NVS1 main-loop top (8169), which returns to it, so the firmware does its own work:
- a slider move runs the LARC slider handler (8614), which clamps to the program's range, writes the register slot of that page's storage index and rebuilds incrementally. The DSP keeps running;
- MODE ENH forces a full reprogram (A791 with 3C72 = FF): about 0.52 s of 8080 time with up to three DSP halts of ≤ 42 ms, which cut the tail;
- DYN DECAY re-arms the decay state (3C51 = C0); DECAY OPT needs no follow-up.

`EngineConfig::optionMask`/`optionBits` (`Machine::setBootOptions`) force option bits as the program loads, before its first build, so they cost no reprogram. See [docs/parameters.md](../docs/parameters.md) for what each slider and option does.

`Machine::sliderValues(out)` reads back the firmware's slider registers (3CA3 + 6 × storage index + slider, the clamped values the handler stored) as `out[page-1][slider]`, with absent pages zero, and `Machine::options()` reads the option bits (3CCD). `Engine` forwards both; the pedal reports them to its editor.

`Engine::setOutMix(k, mix)` changes output k's DAC mix (as `EngineConfig::outMix`) from the next DSP frame. It is not a firmware control; the pedal uses it for its editor's mono-output override.

## How it works

For the machine model, see [docs/dsp_microcode.md](../docs/dsp_microcode.md): the shared 30.72 MHz timeline, the 8080's wait state on every off-board machine cycle (`Machine::kWaitStates`) and the WCS stall on SBC access. The ARU multiply-accumulate and the FPC quantisation follow [docs/aru_fpc.md](../docs/aru_fpc.md): the MAC is bit-exact to the ROM's ARU self-test, and the FPC has 4 gain ranges (step 16, 8, 4 or 2).

Headroom ports: IN 08/09 return the (pre-emphasised) input's gain-ranging level as the AIN/FPC headroom registers would: a 5-bit thermometer code at 7FFFh/4000h/2000h/1000h/0800h, power-of-two stand-ins for the AIN's 5.0/2.24/1.12/0.56/0.28 V comparators. The code is held until the firmware reads the port. The firmware's input-level detector, which drives DECAY OPT and DYN DECAY, depends on them. The bit order is `[INFERENCE]` (see [docs/parameters.md](../docs/parameters.md) §4.1).

The source files are:
- `src/i8080.*`: the 8080 CPU;
- `src/dsp.*`: the DSP (each WCS step is decoded once, when it is written; `Dsp::run` executes batches of steps with the hot state in locals) and the floating-point converter (FPC);
- `src/machine.*`: the memory map, ports and scheduler; ROMs and DMEM are caller-owned;
- `src/resampler.*`: the shared kernel table and the streaming resampler;
- `src/emphasis.h`: the AIN pre-emphasis and AOUT de-emphasis shelf (first-order, bilinear);
- `src/engine.*`: the real-time engine (pre-emphasis, resample in, machine, resample out, de-emphasis, fixed latency). It allocates nothing, so the host CLI and the Daisy firmware share it;
- `src/romfile.*`: host-side ROM file loading;
- `src/audio.*`: WAV I/O;
- `src/main.cpp`: the command-line interface.

## Undefined behaviour

An audit on 2026-10-02 found no undefined behaviour that changes the output. Details:

- **Implementation-defined behaviour the bit-exact model relies on.** The build is C++17, where both of these are left to the compiler. GCC and Clang (host and `arm-none-eabi`) define them as the model needs, and C++20 requires them. A compiler that chose otherwise would break the ARU's bit-exactness.
  - Right shift of a negative value is arithmetic: the MAC (`a >> 3`, `x >> 1`, `x >> 2` in `Dsp::runLoop`), `fpcQuantize` and `fpcHeadroom` (`x >> 15`, `x >> shift`), and 24-bit WAV input (`int32_t(u) >> 8` in `readWav`).
  - Narrowing an out-of-range value to a signed type wraps: the X-in register (`int16_t(...)` in `Machine::out`, ports 06/07) and 16-bit WAV input (`int16_t(le16(s))`). Every narrowing is an explicit cast.
  - `fpcQuantize` multiplies by `1 << shift` instead of shifting a negative value left, which would be undefined in C++17.
- **The MAC cannot overflow `int32_t`.** \|acc\| ≤ 2¹⁸ and \|sHi·x\| ≤ 15·2¹⁵, so the sum before `sat19` stays below 2²⁰.
- **One formal case, with no effect.** In `StreamResampler::gather` and `scatter` (`src/resampler.h`), each paired-tap loop advances `kp += 2 * P` once more after its last pass. That leaves `kp` up to 37 floats past the end of the 8026-entry kernel table. Pointer arithmetic beyond one-past-the-end is undefined, but `kp` is never dereferenced there: the highest index loaded is 8024. Computing `kp` from `m` inside the loop would make it strictly legal.
- **The pedal hands the engine over without a data race.** `gState` (atomic) passes ownership between the main loop and the audio callback, and the callback is an interrupt that the main loop cannot preempt (`daisy/src/lex224_pedal.cpp`).

How it was checked:
- `ctest`, all five tests, built with `-fsanitize=undefined,address`: pass.
- `lex224emu --quad` on all 22 programs, with stereo full-scale input (±1 square, clipped noise, ±DC, sweep) at 48 kHz and 44.1 kHz, built with GCC's `-fsanitize=undefined,float-cast-overflow,address` and `-D_GLIBCXX_ASSERTIONS`: no reports.
- A throwaway driver ran all 22 programs, 2 s each, through `Engine<1,1>` (the pedal's configuration, `outMix` 0x05, 48 kHz) and `Engine<2,4>` (quad, 44.1 kHz). It moved a slider on every page every 100 ms and flipped MODE ENH, DECAY OPT and DYN DECAY. There were no reports under GCC's undefined-behaviour and address sanitizers, Clang's (plus `implicit-integer-truncation`), or Clang's `-fsanitize=memory` (uninitialised reads).
- The same driver's output was bit-identical across GCC 16 at `-O0`, `-O2` and `-O3 -ftrivial-auto-var-init=pattern`, and Clang at `-O2` and `-O3 -ftrivial-auto-var-init=pattern`. Undefined behaviour that a compiler exploits usually shows up as a difference between optimisation levels or compilers, and an uninitialised stack read as a difference under pattern initialisation.
- The ARM pedal build was not run under sanitizers. It compiles the same source with GCC 13.3.

To repeat the sanitizer run (the directory is outside the repository because only `build/` directories are ignored):

```
cmake -S emu -B /tmp/emu-san -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS="-fsanitize=undefined,float-cast-overflow,address -fno-sanitize-recover=all -fno-omit-frame-pointer -D_GLIBCXX_ASSERTIONS"
cmake --build /tmp/emu-san && ctest --test-dir /tmp/emu-san --output-on-failure
```

**Host and pedal renders are not bit-identical.** This is not undefined behaviour: the float stages round differently. The pedal builds in GNU mode, where GCC fuses multiply-adds, and the Cortex-M7 has a fused multiply-add instruction: compiled for the Seed (`-O3 -mcpu=cortex-m7 -mfpu=fpv5-d16`), `engine.cpp` contains 104 fused operations in the resampler and emphasis filters. The baseline x86-64 target has no such instruction. Where a last-bit difference changes a rounded input word, the machine's sensitivity to its input takes the two renders apart `[INFERENCE]`. Compare pedal and host output with `tools/wav_compare.py`.
