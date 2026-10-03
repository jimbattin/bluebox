# lex224 Terrarium pedal

This is firmware for a Daisy Seed in a PedalPCB Terrarium. It runs the `emu/src` engine in real time: the real 224XL V8.2 firmware on an emulated 8080, plus the DSP microcode that firmware loads. The engine is the one the host CLI `lex224emu` uses (see [emu/README.md](../emu/README.md)), and it allocates no memory. Its float stages round differently on the device, because GCC fuses multiply-adds for the Cortex-M7, so pedal and host renders are not bit-identical (see [emu/README.md](../emu/README.md#undefined-behaviour)).

## Build and flash

One-time setup, from the repo root (the library build needs the ARM toolchain on `PATH`):

```
git submodule update --init --recursive   # daisy/libDaisy (libDaisy v8.1.0) and its drivers
make -C daisy/libDaisy                    # daisy/libDaisy/build/libdaisy.a, once per checkout
```

Then:

```
export PATH=$HOME/.arm/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi/bin:$PATH
make -C daisy                # build/lex224.bin
make -C daisy program-dfu    # dfu-util -a 0 -s 0x90040000:leave -D build/lex224.bin -d ,0483:df11
make -C daisy EMBED_ROMS=0 && make -C daisy EMBED_ROMS=0 program-dfu   # without the ROM images: build_noroms/lex224.bin
```

With the Seed in DFU mode, `daisy/tools/flash_when_ready.sh [FILE.bin]` does the same flash. It waits up to 120 s for the DFU device first, so it can be started before entering DFU. FILE defaults to `daisy/build/lex224.bin`.

- libDaisy is the git submodule `daisy/libDaisy`, pinned to v8.1.0 (`LIBDAISY_DIR` overrides it). `src/terrarium.h` is vendored unchanged from [PedalPCB/Terrarium](https://github.com/PedalPCB/Terrarium) commit `cd6c80d`.
- `APP_TYPE = BOOT_SRAM`: the Daisy bootloader copies the app from QSPI into SRAM.
- The build uses `-O3` and `-std=gnu++17`, without `-ffast-math`. `main()` sets FPSCR.FZ (flush denormals to zero).
- The ROMs are linked in by `src/roms.s`, which `.incbin`s `roms/224XL_v8_21/*.BIN` as `lex_rom_sbc` (0x1800 bytes) and `lex_rom_nvs` (0x8000 bytes). `EMBED_ROMS=0` leaves `roms.s` out (`LEX_EMBED_ROMS=0`) and builds into `build_noroms/`, so switching never links stale objects; that firmware runs once the editor has flashed all eleven ROMs (see [ROMs](#roms)).
- **DFU must be entered by hand:** hold BOOT, tap RESET, release BOOT. The running firmware cannot put itself into DFU. The device then shows as `0483:df11`; while the firmware runs it is `0483:5740`, a USB MIDI device named "Daisy Seed Built In" (see [Editor and USB log](#editor-and-usb-log)).
- Linker warnings about `_close`/`_read`/… stubs and the RWX LOAD segment come from newlib and libDaisy's BOOT_SRAM layout, not from this code.

## Controls

Every pedal control below has been checked on the device. The [BARC editor](#editor-and-usb-log)'s SysEx link has been checked on the device from the host; the browser editor itself has not been run against the device yet.

### Audio

- **Input:** the left input is the mono source. It feeds both DSP inputs (L and R).
- **Outputs:** both codec outputs carry the same mono signal (see [Mono output](#mono-output)); which DACs make it up is per program, settable from the editor.

### Knobs

Hold FS2 to switch the knobs to the secondary layer; release it to return to the primary layer.

|Knob|Primary|Secondary (FS2 held)|
|---|---|---|
|1|Dry/wet mix (see [Dry/wet level](#drywet-level))|PREDELAY|
|2|LF DECAY|DIFFUSION|
|3|MID DECAY|DEFINITION|
|4|CROSSOVER|CHORUS|
|5|TREBLE DECAY|HF BANDWIDTH|
|6|DEPTH|LF STOP DCY|

Apart from the mix, each knob is a slider of the 224XL's LARC remote:

|Parameter|LARC page|What it does|
|---|---|---|
|LF DECAY|1|Decay time of the low band, relative to MID DECAY|
|MID DECAY|1|The main decay time. On CONCERT HALL, RT60 goes from 1.8 s (knob at 1/8) to 4.6 s (3/4)|
|CROSSOVER|1|The frequency below which the tail decays at the LF DECAY rate instead of the MID DECAY rate: about 160 Hz at 0, 1.5 kHz at 1/4, 3.6 kHz at 1/2 and 6.6 kHz at 3/4. It does nothing while LF DECAY equals MID DECAY, which is the factory setting of ROOM, SMALL ROOM, PLATE, SMALL PLATE, CD PLATE A and B, PLATE/PLATE, PLATE/CHORUS, RICH PLATE, INVERSE ROOM, RICH SPLIT and the left half of PLATE/HALL. Everything below 1 kHz happens in the first quarter of travel (92% of the change); the rest moves only the 2–4 kHz bands. It is loudest with LF DECAY (knob 2) at full and MID DECAY (knob 3) at 1/4: on CONCERT HALL the 125/250/500/1000 Hz RT60 then goes from 8.0/3.9/2.2/1.9 s at 0 to 14.5/15.3/14.8/11.0 s at 1/4. With LF DECAY at 0 and MID DECAY at 3/4 it works the other way, from 2.7–4.9 s at 0 to 0.8–0.9 s at 3/4. At CONCERT HALL's factory LF/MID, 125–250 Hz RT60 is only 2.3 s at 0 and 2.8 s at 3/4. The last 0.2% of travel (slider FF) switches the LF band off, so the whole tail then decays at the MID DECAY rate|
|TREBLE DECAY|1|How fast the highs die away in the tail. At full travel every band drops to about 1 s|
|DEPTH|1|Sets four internal gains from curve tables; its audible effect has not been characterised|
|PREDELAY|1|Delay before the reverb. On CONCERT HALL, about 82 ms at 1/4 and 705 ms at 3/4. A change ramps the delayed signal down and back up instead of jumping|
|DIFFUSION|3|Gain of four input allpass sections: how much the input is smeared before it reaches the tail|
|DEFINITION|3|Above half travel, thins the tail into fewer, more distinct echoes; the decay time stays the same|
|CHORUS|3|Speed of the MODE ENH modulation. It does nothing with switch 1 (MODE ENH) off|
|HF BANDWIDTH|3|High-frequency cutoff|
|LF STOP DCY|3|Only acts with DYN DECAY on, which no factory program uses and only the editor can turn on|

More detail is in [docs/parameters.md](../docs/parameters.md).

How the knobs behave:
- **Takeover.** After power-up, a program change, and pressing or releasing FS2, every knob is parked: it changes nothing until it moves 2% of its travel. So releasing FS2 leaves the primary parameters where they were, even though the knobs now point elsewhere; each takes over again once it is moved. When the editor moves a knob's slider (or the mix), that knob alone is parked the same way.
- **Range.** Full travel covers the whole LARC slider range. The knob goes through the emulated firmware's own slider handler (see [emu/README.md](../emu/README.md#live-controls)), so each program limits it to its own range, as the LARC would.
- **Program changes** start from the program's factory settings, not from the knob positions.
- **Split programs** (HALL/HALL, PLATE/PLATE, PLATE/HALL, RICH SPLIT) have separate left and right reverbs. A page-1 knob moves both together.
- **Missing pages.** CHORUS&ECHO has no page 1; RES CHORDS and M BAND DELAY have neither page 1 nor page 3. Knobs whose page is missing do nothing. The mix knob always works.

### Footswitches

|Switch|Tap|Hold|
|---|---|---|
|FS1|Bypass on/off. Bypass passes the clean dry signal; LED1 is lit while the effect is active|—|
|FS2|Next program in the current bank, wrapping to the bank's first (acts on release)|Secondary knob layer while held. If a secondary knob took over during the hold, the release does not change the program|

### Toggle switches

|Switch|Up|Down|
|---|---|---|
|1|MODE ENH on: slow random modulation of delay taps, which smooths out ringing (speed: CHORUS)|MODE ENH off|
|2|DECAY OPT on: once the input stops, the tail ends about 2.6 dB quieter|DECAY OPT off|
|3, 4|Bank select (see [Banks](#banks))|Bank select|

Every program loads with its own MODE ENH and DECAY OPT settings (the last column of the [Banks](#banks) table), whatever the positions of switches 1 and 2. Flipping a switch after that sets its option to the switch's new position until the next program load, so a flip toward the setting the program already has changes nothing. A flip made while a program loads is applied as soon as it runs. Flipping switch 1 while a program runs reprograms the DSP, as on the 224XL: a short glitch and the tail is cut. Switch 2 changes without a glitch.

The editor can also set MODE ENH, DECAY OPT and DYN DECAY. No factory program uses DYN DECAY, so every load turns it off. Like a switch flip, an editor setting lasts until the next program load, or until the option's switch is flipped, which sets the option to the switch's position.

Checked on the device (2026-10-03). The switches stayed in one position while the editor loaded CONCERT HALL, CD PLATE A, RES CHORDS, INVERSE ROOM and CONCERT HALL again. The options came up as each program's factory setting (C0, 40, 00, 80, C0). Editor changes made before a load (DYN DECAY on, MODE ENH off) were dropped by that load. A switch 1 flip set MODE ENH live, and the next FS2 load returned to that program's own setting.

### Banks

Switches 3 and 4 select one of four banks. FS2 steps through the programs of the current bank. A bank change loads its first program once the levers have been still for 300 ms, so passing through another lever combination does not load it. The levers act only when they move: a program picked in the editor stays loaded, whatever bank the levers show, and FS2 then steps through that program's bank.

|Bank|Switch 3|Switch 4|Program (FS2 order)|Factory switch 1 / 2|
|---|---|---|---|---|
|1|Down|Down|CONCERT HALL|Up / Up|
|1|Down|Down|BRIGHT HALL|Up / Up|
|1|Down|Down|DARK HALL|Up / Up|
|1|Down|Down|ROOM|Up / Up|
|1|Down|Down|SMALL ROOM|Up / Up|
|1|Down|Down|CHAMBER|Up / Up|
|2|Up|Down|RICH CHAMBER|Up / Up|
|2|Up|Down|PLATE|Up / Up|
|2|Up|Down|SMALL PLATE|Up / Up|
|2|Up|Down|CD PLATE A|Up / Down|
|2|Up|Down|CD PLATE B|Up / Down|
|2|Up|Down|CHORUS&ECHO|Up / Down|
|3|Down|Up|RES CHORDS|Down / Down|
|3|Down|Up|M BAND DELAY|Down / Down|
|3|Down|Up|HALL/HALL|Up / Up|
|3|Down|Up|PLATE/PLATE|Up / Up|
|3|Down|Up|PLATE/HALL|Up / Up|
|3|Down|Up|PLATE/CHORUS|Up / Up|
|4|Up|Up|RICH PLATE|Up / Up|
|4|Up|Up|DARK CHAMBER|Up / Up|
|4|Up|Up|INVERSE ROOM|Down / Up|
|4|Up|Up|RICH SPLIT|Down / Up|

This is ROM-table order, as `lex224emu --list` prints it. The pedal powers up on the first program of the selected bank; the current program is not saved.

A program change re-boots the whole emulated machine (reset, 8080 boot, program select, microcode load). The wet output is muted meanwhile: 0.77–0.82 s on the device.

A Cortex-M7 fault, an audio stall or a hang restarts the pedal (see [Lockups](#lockups)). The program the bank levers select then loads, as at power-on; the bypass state and the current program are not kept.

### LEDs

|LED|Meaning|
|---|---|
|LED1|Lit while the effect is active; off in bypass|
|LED2|While a program runs: N short flashes for program N of its bank (150 ms on, 150 ms off), a 1.5 s pause, repeated. Lit steadily while a program loads. Blinks at 4 Hz if a program fails to load or the DSP halts, at 1 Hz while a ROM slot is empty (only in the `EMBED_ROMS=0` build; the output is then dry only)|

The control logic (`src/pedal_controls.h`) has no libDaisy dependency. Its host tests run with `make -C daisy test` (no toolchain needed beyond the host `g++`).

## Dry/wet level

The mix knob (knob 1, primary layer) keeps the output at the dry level across its whole travel:
- **Equal-power law.** Dry is scaled by cos and wet by sin of the knob angle (0–90°, `mixGains` in `src/pedal_controls.h`). The wet output is nearly uncorrelated with the dry input (\|r\| ≤ 0.18 on guitar), so a linear crossfade dipped about 3 dB in the middle; equal power does not. Full dry is exactly the input, the same as bypass.
- **Per-program wet trim** (`kWetTrim` in `src/lex224_pedal.cpp`). The factory programs' wet outputs are 0.3–8.9 dB quieter than the dry input (RES CHORDS is 6.5 dB louder). Each program's trim is the BS.1770 loudness difference between dry and wet for `guitar.wav` (left) through the pedal's engine config at factory slider settings, so it measures each program's own [mono output](#mono-output). It is level-independent: within 0.15 dB between −12 and −24 dBFS peak input. Slider moves change the wet level (longer decays are louder), so the match is exact only at the factory settings. An editor override of the mono output keeps the program's trim, so it can shift the wet level.

Measured on the same renders with the pedal's output law, every program's blend stays within −0.9 to +0.4 dB of dry from full dry to full wet. On the device, by ear, full wet matches bypass and the middle of the knob no longer dips.

The trim is applied after the engine, folded into the per-block wet gain, so it costs nothing per sample. It scales the emulated DSP's own idle noise with the wet signal. At full wet, with −100 dBFS white noise at the input, that floor rises from −86…−102 dBFS to −82…−98 dBFS, depending on the program. INVERSE ROOM and ROOM are the noisiest; SMALL ROOM rises the most (+8.9 dB); RES CHORDS drops 6.5 dB. The dry path and the input headroom are unchanged.

## Mono output

`EngineConfig::outMix` is a bitmask of the DACs to average (A = 1, B = 2, C = 4, D = 8). By default the pedal uses:
- **mean of A and C** (`0x05`) for eight programs that write DAC A together with D and B together with C: SMALL ROOM, PLATE, SMALL PLATE, CD PLATE B, CHORUS&ECHO, RES CHORDS, M BAND DELAY and RICH PLATE. They have only two distinct outputs, with their voices split between A and C; M BAND DELAY's A is 11 dB below its C;
- **mean of all four** (`0x0F`) for PLATE/CHORUS, a user choice (below);
- **DAC A alone** (`0x01`) for the other 13: the 12 programs with four distinct outputs, and CD PLATE A (below).

The original Lexicon 224 manual's mono-in/mono-out table uses A, or A + C for P2 Vocal Plate, P4 Acoustic Chamber, P5 Percussion Plate and P9 Chorus A of the v4.4 set. Its programs with A = D and B = C are P2, P5, P8 Constant Density Plate A and P9, so the 224's choice is not the routing rule either: P8 uses A alone, and P4 uses A + C. Source: [UA Lexicon 224 manual](https://help.uaudio.com/hc/en-us/articles/4419497193492-Lexicon-224-Digital-Reverb-Manual) ("Mono/Stereo Operation", "Rear Outs", "MIMO Program Outputs"). The V8.2 ROM has no such table: its only text containing "OUTPUT" is `OUTPUT MUTE ACTIVATED`, and the fifth byte of each factory-program entry is a slider-page index (see [docs/rom_map.md](../docs/rom_map.md) §4). Without a real 224XL, the list above is `[INFERENCE]`, checked on the emulator instead.

**Emulator check** (2026-10-03): `guitar.wav` (left, 20 s) through `lex224emu --quad`. "Ripple" is the spectrum of the mean of A and C against that of an uncorrelated pair with the same levels, in 1/12-octave bands from 100 Hz to 10 kHz; a comb filter shows as deep notches.

|Program (A = D, B = C)|A, C (dBFS RMS)|Largest A–C correlation|Ripple of the mean|
|---|---|---|---|
|SMALL ROOM|−24.4, −24.9|0.53 at 1.6 ms|−7.4 … +2.7 dB|
|PLATE|−23.1, −20.8|0.32 at 3.8 ms|−5.6 … +2.6 dB|
|SMALL PLATE|−20.9, −21.7|0.28 at 8.4 ms|−5.0 … +2.4 dB|
|**CD PLATE A**|−20.6, −20.2|**−0.90 at 11.7 ms**|**−15.6 … +2.8 dB**|
|CD PLATE B|−18.9, −19.2|0.63 at 16.4 ms|−6.5 … +2.6 dB|
|CHORUS&ECHO|−21.9, −20.5|0.27 at 0.1 ms|−2.7 … +2.0 dB|
|RES CHORDS|−11.4, −12.8|0.41 at 0.2 ms|−0.8 … +2.7 dB|
|M BAND DELAY|−33.0, −22.1|0.44 at 0 ms|+0.1 … +2.5 dB|
|RICH PLATE|−24.4, −24.6|−0.23 at 1.5 ms|−3.4 … +2.5 dB|

- A + C on the four-output programs would give notches of −4.0 to −9.8 dB, so the eight kept programs fall within what any summed pair here shows.
- CD PLATE A is the exception. Its C is largely its A, inverted and 11.7 ms later, so their mean is a comb filter. It switched to A alone on 2026-10-03, which agrees with the 224 manual's choice for its CD Plate A. Its trim went from 2.3 dB to 0.3 dB: the BS.1770 loudness of the mean is 1.96 dB below that of A alone on the same render.
- PLATE/CHORUS has four distinct outputs and switched from A alone to the mean of all four on 2026-10-03, by user choice. On `guitar.wav` (left, whole file, −12 dBFS peak) through `lex224emu --quad`, A, B, C and D play at −33.9, −34.4, −33.8 and −34.7 dBFS RMS, the largest pair correlation is −0.36 (A–C at 19.2 ms), and the mean's ripple against four uncorrelated outputs of the same levels is −3.3 … +3.6 dB, so it does not comb-filter. Its trim went from 3.3 dB to 8.8 dB: the BS.1770 loudness of the mean is 5.4 dB below that of A alone on the same render, which itself reproduces the old 3.3 dB. The new trim is 8.7 dB at −24 dBFS and 8.6 dB at 0 dBFS peak.
- **Level:** the mean of an uncorrelated pair plays about 3 dB below either output (1.3–3.5 dB on these renders), and PLATE/CHORUS's mean of four 5.4 dB below its A. The [wet trim](#drywet-level) is measured through each program's own mix and makes this up, so no extra level is needed.

The averaging happens before the output resampler. The resampler is linear, so this equals resampling A and C separately and averaging, within 2.4e-7. The average cannot clip.

### Editor override

The editor's MONO OUT keys replace the default for the loaded program:

|Key|`outMix`|
|---|---|
|DEFAULT|the program's own (above); clears an override|
|A|`0x01`|
|C|`0x04`|
|A+C|`0x05`|
|A+B+C+D|`0x0F`|

- **Protocol:** OUTMIX (`0B`) carries the choice, `00` for DEFAULT; the parser refuses any other mask. STATE ends with `outMix` (the program's override, 0 = none) and `outDefault` (its default mask), so the editor can label DEFAULT and light the right key (`src/editor_protocol.h`).
- **Storage:** one entry per program ID in `OutMixTable` (`src/pedal_controls.h`), saved in the QSPI sector at chip offset 0x7FB000, right after the [ROM slots](#roms). The record is the magic `OXM1`, the 256 entries and a CRC-32 of both, little-endian. An erased or corrupt sector reads as DEFAULT for every program, and an entry that is not a menu choice reads as DEFAULT. The sector lies in the same 64 KiB chunk as the ROM store, so it should survive `program-dfu` the same way `[INFERENCE: not checked on the device]`.
- **Path:** the main loop handles OUTMIX itself rather than queueing it for the callback, because it writes flash. It stores the choice, erases and rewrites the sector and reads it back (the erase blocks the main loop for tens of ms; audio keeps running), then hands the program's effective mix to the callback, which applies it with `Engine::setOutMix` from the next DSP frame. There is no crossfade: the switch is a step in the wet signal, like a MODE ENH flip. A choice equal to the default (A on CONCERT HALL) is still stored as an override; only DEFAULT clears one.
- **Level:** `kWetTrim` stays the program's, measured through its default output, so an override can play a few dB louder or quieter (M BAND DELAY's A is 11 dB below its C).
- **Log:** `outmix store: N overrides` or `outmix store: empty` at boot; `PLATE out C, saved` on a change (`PLATE out A+C (default), saved` for DEFAULT, `not saved (flash error)` if the write or read-back fails); the program line reads `out C (editor)` while an override applies.
- **Checked** (2026-10-03) on the host only: `engine_test` (a live switch to C matches the separate C output, max difference 0), the `OutMixTable` record and protocol tests, the firmware build, and the editor against a simulated pedal in headless Chromium (DEFAULT labelled `DEFAULT (A+C)` on PLATE and `DEFAULT (A)` on CONCERT HALL, the comb-filter reason on CD PLATE A, the lit key and panel status following STATE, C sending `F0 7D 42 01 0B 04 F7`). Not checked on the device: the OUTMIX round trip, the save across a power cycle and `program-dfu`, how the switch sounds, and the load.

## Editor and USB log

The Seed's own USB port is a USB MIDI device. libDaisy v8.1.0 runs one USB class per port (`MidiUsbTransport` re-inits it with MIDI descriptors), so the text log that used to go over USB CDC now travels as SysEx too.

### BARC editor

[`editor/`](../editor) at the repo root is a browser editor (TypeScript, Preact, built and served with [Bun](https://bun.sh)) styled after the 224XL's remote and branded **Bluebox BARC**. It needs Web MIDI with SysEx, so Chrome, Edge or Firefox (Chrome 124+ / Firefox 120+ for the vertical faders); the browser asks for permission on first use. From the repo root:

```
make -C editor serve   # dev server with hot reload: http://localhost:3000 (PORT=… to change)
make -C editor html    # editor/dist/editor.html, one self-contained file
make -C editor test    # protocol byte vectors (bun test); `check` runs tsc
make -C editor fmt     # prettier --write; `check` also runs prettier --check
```

The served page on localhost is the supported path. Headless Chromium links to the pedal there once the page has the `midi` and `midi-sysex` permissions (2026-10-03). Chrome treats `file://` as a secure context, so the single-file build should get Web MIDI too `[INFERENCE: not run]`.

Firefox asks to install a site-permission add-on on first use. It denies MIDI access when no MIDI device is connected, and it has no hot-plug ([Bugzilla 1802149](https://bugzilla.mozilla.org/show_bug.cgi?id=1802149)), so connect the pedal first, and reload the page after replugging it or after a pedal restart. The single-file build gets no Web MIDI from `file://` in Firefox ([Bugzilla 1850446](https://bugzilla.mozilla.org/show_bug.cgi?id=1850446)): serve it.

It picks the first MIDI ports named like "Daisy" (two selects override this) and shows LINKED once the pedal answers. It controls:
- **Programs:** all 22, in four bank columns matching the levers (6/6/6/4).
- **Sliders:** every page the loaded program has, six faders per page with the 224XL's short label, a readable name and a help line. Values are raw slider positions 0–255; the firmware clamps them to the program's range, and the fader shows the clamped value 0.4 s after the last move.
- **Page keys** with readable titles. Programs of one family show the same keys, and a page the program lacks stays visible but greyed out (CHAMBER's pre-echo pages, CHORUS&ECHO's reverb). The families are reverb, split reverb, chorus, RES CHORDS and M BAND DELAY. All keys are greyed out while a program loads. Page 6, FINE DELAYS, is inactive in the ROM and hidden.
- **Mix, bypass, MODE ENH, DECAY OPT, DYN DECAY**, with key LEDs that show the pedal's state.
- **Mono out:** DEFAULT, A, C, A+C, A+B+C+D for the loaded program, saved on the pedal; hover help on each key and a collapsible MONO OUTPUT panel explain the choices (see [Mono output](#mono-output)).
- **Log:** a collapsible panel with the last 200 log lines.
- **ROMs:** a collapsible circuit board with one DIP socket per ROM (see [ROMs](#roms)).

The pedal is the source of truth. Every 20 ms the main loop asks the callback (which owns the engine) for a snapshot of the slider registers, options and mix (STATE also carries the program's mono-output override and default), and sends what changed; everything is resent once a second and on the editor's HELLO. Commands go USB IRQ → byte queue → main-loop parser → command queue → audio callback, which applies them through the same paths as the knobs and switches (`Machine::moveSlider`/`setOption`). OUTMIX is the exception: the main loop stores and saves it, then hands the mix to the callback. A program selection boots the program from the main loop, like FS2. The SysEx format is in `src/editor_protocol.h` (manufacturer ID 7D, the non-commercial ID); `tests/editor_protocol_test.cpp` and [`editor/src/protocol.test.ts`](../editor/src/protocol.test.ts) check the same byte vectors.

### ROMs

The engine runs from a RAM copy of the eleven ROMs (`gRomSbc`/`gRomNvs` in AXI SRAM), built at boot and after every ROM change by `loadRomImage()`. Per slot it takes, in order:
1. the image flashed into the slot's QSPI sector, if its byte sum is the socket index (the ROM's own checksum test; an erased sector sums to 0, never a socket index);
2. the built-in image (default build only);
3. nothing: the slot is empty, the pedal stays in the NoRoms state (dry output, LED2 at 1 Hz, STATE runState 3) and the editor shows `NO ROMS - FLASH THEM IN THE ROM PANEL`.

Slot n (0–2 SBC1–3, 2716, 2 KiB; 3–10 NVS1–8, 2732, 4 KiB) is the 4 KiB sector at QSPI chip offset `0x7F0000 + n × 0x1000`, in the last 64 KiB of the 8 MiB flash. The Daisy bootloader stores apps from 0x40000 and erases only the 64 KiB chunks a program covers, so the store survives `program-dfu` (checked below). The next sector, 0x7FB000, holds the mono-output overrides ([Editor override](#editor-override)).

The editor's ROMS panel draws the SBC (3 × 2716) and NVS (8 × 2732) boards. Each chip's label shows the version of the image in it (`V8.2` for the CRC-32s of `roms/224XL_v8_21/`, else `UNKNOWN`), its slot and CRC-32; below it, `FLASHED`, `BUILT-IN` or `—`. Clicking a socket or dropping a file on it flashes it:
- The editor checks the length and byte sum first ("This is the SBC2 image (byte sum 2), not SBC1"), and asks before flashing an image of unknown CRC. Other firmware versions are allowed but may not boot (`LOAD FAILED`).
- ROM_BEGIN, then 64-byte ROM_DATA chunks, each acknowledged with the next offset wanted; a lost chunk or acknowledgement is resent after 1 s (3 attempts). ROM_COMMIT carries the CRC-32 of the whole image; the pedal checks it and the byte sum, erases the sector, writes it, reads it back and answers ROM_RESULT, then reloads its ROM image and boots the current program again (wet muted 0.8 s).
- A flashed slot has a REVERT key (ERASE in the `EMBED_ROMS=0` build): it erases the sector, so the built-in image (or nothing) takes over.

The pedal reports every slot's state and CRC in a ROMS message at boot, on HELLO and after each change. Flash work runs in the main loop while the engine keeps running from RAM (it reads nothing from QSPI); a sector erase blocks the loop for tens of milliseconds.

**Checked on the device** (2026-10-03, the editor in headless Chromium on localhost):
- The `EMBED_ROMS=0` build booted into NoRoms: `roms -----------`, `NO ROMS - FLASH THEM IN THE ROM PANEL` and eleven EMPTY sockets.
- All eleven V8.2 files flashed one after another. The log went `roms F----------` … `roms FFFFFFFFFFF`, and the pedal then booted the levers' program with no `event` line. Each socket showed `V8.2`, its CRC-32 and FLASHED.
- ERASE on NVS8 put the pedal back in NoRoms; flashing NVS8 again booted it.
- CONCERT HALL from the flashed images, editor closed: 672 average, 708–717 max permille, the same average as from the built-in images ([Performance](#performance-measured-on-the-device-480-mhz)).
- The store survived a power cycle (the next boot read `roms FFFFFFFFFFF`) and a `program-dfu` of the default build (all eleven still FLASHED).
- REVERT on each slot went `BFFFFFFFFFF` … `BBBBBBBBBBB`, rebooting the program after each one, and left every socket BUILT-IN.

### USB log

Once a second, and on every program load, the firmware sends a log line as a LOG SysEx message:

```
lex224 sysclk 480 MHz
roms BBBBBBBBBBB (F flash, B built-in, - empty)
outmix store: empty
program CHORUS&ECHO (id 0C) loop 100 steps, out A+C, latency 184, boot 771 ms, wet +4.4 dB
CHORUS&ECHO load avg 744 max 770 permille
event 1: cpu fault at 15 s, prog 0 state 1 blocks 13733, rsr 01460000
  pc 240029F6 lr 2400295B xpsr 61000000 cfsr 00000082 hfsr 00000000 mmfar 907FFFFF bfar 907FFFFF exc FFFFFFE9
  dma lisr 00000000 s0 0002555F/29 s1 0002551F/31 sai 00010000 00000000
```

- `load` comes from libDaisy's `CpuLoadMeter` over the audio callback. It is the fraction of the 1 ms block period (48 samples at 48 kHz), in permille, and is reset every second.
- `latency` is the engine's fixed latency in samples at 48 kHz.
- `out` is the program's [mono output](#mono-output); `(editor)` marks an [editor override](#editor-override).
- `boot` is wall time for `Engine::start`.
- `wet` is the program's wet trim (see [Dry/wet level](#drywet-level)).
- `event` lines appear once a restart has been recorded since power-on (`src/crash_trace.h`). The three event lines then take turns with the load line, one line per second. Only the last event is kept, and a power cycle clears it.
  - Line 1: the count since power-on; the kind (`cpu fault`, `audio stall`, or `unexplained reset` for a watchdog, reset-pin or brown-out reset); seconds since the previous boot; the program index and state running; audio blocks completed; and `RCC->RSR` at the following boot (`04000000` = IWDG, `00200000` = brown-out). Entering DFU by hand (BOOT + RESET) and flashing is also recorded as an `unexplained reset` (seen with `rsr 01460000`).
  - Line 2, for a fault: the stacked PC and LR (`arm-none-eabi-addr2line -e daisy/build/lex224.elf <pc>`), xPSR (bits 8:0: 0 = main loop, 27/28 = the SAI1 DMA interrupts, the receive one runs the audio callback), CFSR/HFSR/MMFAR/BFAR and EXC_RETURN.
  - Line 3, for a fault or stall: the SAI1 DMA streams' CR/NDTR and SAI1 A/B status.

The editor's log panel shows these lines. Without the editor:

```
uv run daisy/tools/cdc_read.py [SECONDS]    # default 30; needs the repo's uv environment (`uv sync`) and libusb
```

- It reads the MIDI streaming endpoint through libusb and prints the LOG messages. The kernel's audio driver is detached while it reads and reattached afterwards, so close the editor first. For a CDC device (the [engine bench](#engine-bench)) it reads the text lines instead.
- It reconnects whenever the device re-enumerates (reset or reflash).
- Each line is prefixed with the seconds since the last (re)connect.

Sending never blocks. libDaisy's `Logger` was not used, because once a host has read from the port it switches to `TransmitSync`, which waits forever if the host then stops reading; that hung the main loop and, with it, FS2 program switching. Messages are packed into one of two 1 KB buffers and the main loop starts a transfer of the filled one with `UsbHandle::TransmitInternal` when the endpoint is free; a busy endpoint keeps the batch for the next pass, and what does not fit is dropped. libDaisy's `MidiUsbTransport::Tx` is not used: it sends a SysEx's closing F7 as a packet of its own.

With no USB host at all (cable unplugged, 2026-10-03), FS2 stepped through four programs and a bank-lever change loaded the new bank, with LED2's count following. No restart was recorded. Once the cable was plugged back in, the log resumed. The first ~1 KB of messages (the boot lines and about 3 s of load lines) waits in the buffer and arrives at connect; everything after it was dropped.

## Memory

No heap is used: `nm -u` on the objects shows no `malloc`/`free`/`operator new`/`operator delete`. The editor link uses `MidiUsbTransport` directly: libDaisy's `MidiHandler` holds a 256-event FIFO with 128-byte SysEx buffers (about 38 KB), which does not fit in DTCM.

|Item|Where|Size|
|---|---|---|
|`Engine<1,1>` (machine, resamplers, emphasis filters, queues, FIFO, live-control request queue)|DTCM `.bss`|30816 B|
|Resampler kernel table `gKernel`|DTCM (`DTCM_MEM_SECTION`)|32104 B|
|DSP delay memory `gDmem` (64 Kwords)|AXI SRAM, `.sram_bss`|131072 B|
|Editor link: send buffers `gTx` (2 × 1 KB), receive and command queues, parser, snapshots; libDaisy's `midi_usb_handle` (2112 B)|DTCM `.bss`|about 5.4 KB|
|ROM upload staging `gUpload` (`.bss`, because `.sram_bss` is not zeroed)|DTCM `.bss`|4100 B|
|ROM image `gRomSbc` + `gRomNvs`|AXI SRAM, `.sram_bss`|38912 B|
|`crash::gEvent`, `crash::gTrace` (restart record, callback breadcrumbs)|backup SRAM, `.backup_sram.lex`|120 B|
|`crash::gVectors` (vector table copy with the four fault entries; `SCB->VTOR` points here)|DTCM `.bss`|664 B (1 KiB aligned)|

`STM32H750IB_sram_lex224.lds` is libDaisy's `STM32H750IB_sram.lds` with an added `.sram_bss` NOLOAD section. The startup code does not zero it; `Engine::start` clears the DMEM on every program load and `loadRomImage()` writes every byte of the ROM image. With DMEM in AXI SRAM rather than SDRAM, the load was about 80 permille lower. The ROM image sits in the same AXI SRAM as the embedded images (`.rodata`), so 8080 fetches cost the same as before it existed `[INFERENCE: load not yet measured]`.

`crash::gEvent`/`gTrace` survive a reset while the pedal is powered. They link after libDaisy's `boot_info` (`*(.backup_sram*)` follows `*(.backup_sram)` in the linker script), which the bootloader shares and which must stay at 0x38800000.

`configureMpu()` (`src/mpu.h`) replaces libDaisy's MPU setup; see [Lockups](#lockups) for the regions.

Current totals, from the linker's memory report: DTCM 102876 B of 128 KB (the stack shares it); SRAM 353632 B (345 KB) of 480 KB, or 314688 B (307 KB) with `EMBED_ROMS=0`; backup SRAM 132 B.

## Performance (measured on the device, 480 MHz)

Firmware before the bit-exact ARU/FPC model, with the live controls, measured on 9 programs over about 5 minutes that included knob sweeps, FS2 layers and bank changes:
- **Load:** 757–775 permille average, 773–793 max, against a target of a maximum under 850. The average target of under 700 is missed by about 60–75.
- **Switch 1 (MODE ENH) flip:** one callback overruns (3.7–3.9 ms against the 1 ms block); the next second is back to normal. By ear the glitch is barely noticeable.
- **Program change:** 0.77–0.82 s of muted re-boot. **Latency:** 184–201 samples at 48 kHz.

**Current firmware: 8080 wait states (`Machine::kWaitStates` = 1) and the MPU background region** ([Lockups](#lockups)), measured 2026-10-03:
- **Engine bench**, all 22 programs: 653–678 permille average, 688–717 max, engine boot 663–718 ms, no underruns. Stages: `Machine::run` 474, the DSP loop alone 306, 8080 and scheduler 168, the whole engine 692 (722 before the wait states). A DSP step costs 39 cycles with DMEM in AXI SRAM, 43 in SDRAM.
- **Pedal**, CONCERT HALL from the built-in ROM images, controls idle, editor closed, read with `cdc_read.py`: 671–673 average, 723–731 max over about 25 one-second readings on two boots. Against the 710 / 727 baseline below, the average is about 38 lower and the maximum the same.
- **Why the wait states help:** the emulated 8080 now does 16% less work per second of audio (CONCERT HALL: 456 000 instead of 543 000 bus accesses). Counting the wait states costs two instructions per NVS fetch in the ARM `Machine::read`. On the host, 5 boots and 50 s of CONCERT HALL through `Engine<1,1>` took 2.0% fewer instructions and 4.6% fewer cycles.
- A MODE ENH flip runs the same 8080 instructions as before, so its overrun should not change `[INFERENCE]`.
- The MPU change was not measured on its own. It leaves DTCM and AXI SRAM, where the engine's code and data live, on the default map, so it should cost nothing `[INFERENCE]`.

**Bit-exact ARU model with the rewritten DSP loop, before the 8080 wait states** (see the list at the end of this section), measured 2026-09-28:
- **Engine bench**, all 22 programs: 690–716 permille average, 706–734 max, engine boot 699–751 ms, no underruns. The DSP costs 39 cycles per step (56 before the rewrite), and the engine stage 722 permille (795 before).
- **Pedal**, CONCERT HALL with the controls idle for 20 s: 710–711 average, 725–727 max. The live run with knob sweeps and FS2 below has not been repeated with this build.
- **Pedal with the USB MIDI editor link** (2026-10-02), the link answering a host and resending its state every second: CONCERT HALL 706–707 average, 727–730 max; RICH PLATE 710–711 / 729–731. The link costs nothing measurable.

**Bit-exact ARU and 4-range FPC model, before the loop rewrite** ([docs/aru_fpc.md](../docs/aru_fpc.md)), measured 2026-09-28:
- **Engine bench**, all 22 programs: 765–783 permille average, 786–803 max, engine boot 808–868 ms, no underruns. The engine stage costs 795 permille (781 before); the MAC adds two masked terms to the one multiply. That is about +10 average and +8 max against the previous bench.
- **Pedal**, about 5 minutes on all 22 programs with guitar input, knob sweeps on both layers, FS2 program changes and switch flips. The second that contains each program load is left out. Load is 769–793 permille average and 781–817 max; the maximum was RICH PLATE, and PLATE was held for only one second. There were no underruns. Four seconds each held one overrunning callback:
  - 3.55 ms and 3.55 ms on CD PLATE A (5 s apart), and 3.80 ms on RICH PLATE. These match the known MODE ENH reprogram (3.7–3.9 ms before this model) `[INFERENCE: switch flips were not logged]`;
  - 1.39 ms on INVERSE ROOM, cause not attributed.

The per-program table below comes from an earlier build, before the live controls; that build ran about 15–25 permille lower. It steps through the programs with FS2, and leaves out the one-second window that contains a program load (it mixes the muted load time and the previous program).

|Program|Out|Loop|Latency|Boot (ms)|Load avg / max (permille)|
|---|---|---|---|---|---|
|CONCERT HALL|A|105|194|—|745 / 775|
|BRIGHT HALL|A|105|194|813|745 / 769|
|DARK HALL|A|107|197|811|743 / 757|
|ROOM|A|105|194|812|744 / 767|
|SMALL ROOM|A+C|105|194|774|743 / 763|
|CHAMBER|A|100|184|776|747 / 771|
|RICH CHAMBER|A|108|199|821|752 / 774|
|PLATE|A+C|100|184|815|745 / 771|
|SMALL PLATE|A+C|100|184|815|744 / 769|
|CD PLATE A|A+C|100|184|772|744 / 766|
|CD PLATE B|A+C|102|188|771|741 / 760|
|CHORUS&ECHO|A+C|100|184|771|744 / 770|
|RES CHORDS|A+C|100|184|778|747 / 768|
|M BAND DELAY|A+C|100|184|770|738 / 756|
|HALL/HALL|A|102|188|778|745 / 766|
|PLATE/PLATE|A|104|192|781|748 / 765|

The remaining six programs (PLATE/HALL, PLATE/CHORUS, RICH PLATE, DARK CHAMBER, INVERSE ROOM, RICH SPLIT) were measured only with the standalone on-device engine bench ([Engine bench](#engine-bench)). Before the bit-exact ARU model, with the converter emphasis, that bench measured all 22 programs at 756–772 permille average and 773–795 max, with engine boot 753–806 ms and no underruns. The emphasis filters cost 2–3 permille average against the same bench without them (engine stage 778 → 781). The A + C mix adds no measurable load.

The first build measured 1650 permille. The changes that brought it down, all byte-identical in DSP output:
- `Dsp::run(n, untilProt)` runs the microprogram in a batch loop. Hot state stays in locals, and the MR/MW/XFER/ZERO choices use branch-free masks pre-decoded into `DspStep`.
- `Machine::runUntil` computes exact DSP batch sizes between 8080 instructions.
- The resampler gathers from a mirrored ring when upsampling, and scatters in transposed form when downsampling, with the kernel interpolation folded into per-input weights.
- DMEM moved to AXI SRAM (about 80 permille).
- The DSP loop (`Dsp::runLoop`) was rewritten for the bit-exact ARU model, 56 → 39 cycles per step, about −70 permille:
  - the coefficient sign is folded into the `DspStep` terms, so the MAC is three multiply-accumulates, an add and `ssat`;
  - `DspStep` shrank from 40 to 20 bytes: masks are `int8_t`, sign-extended by the load;
  - for regular programs (the first RESET at pc 0–126, or none), the loop walks a pointer from pc to the sample's last step, or to the n-th step, with one compare per step; the general loop remains for the rest;
  - PROT is read once, from the last step, instead of stored every step; 16-bit values stay sign-extended in `int32_t`; the register file's address is kept in a register; I/O steps call out to `Dsp::ioStep`.

  Renders of all 22 programs (four DACs) are byte-identical to the previous loop, and so are renders with only the general loop enabled. Writing the register file before reading it (no `sameMask`) was 2 cycles faster per step in the bench's DSP-only loop but no faster in the engine, so the masked select stays.
- `fpcQuantize` used `__builtin_clz` (checked on all 65536 inputs); it is now a three-way compare for the 4-range FPC ([docs/aru_fpc.md](../docs/aru_fpc.md) §4).

## Lockups

No freeze has been seen on this pedal. The firmware carries the fix found on the sibling rmx16 pedal (the blackbox project, its `daisy/README.md`, Lockups) as a precaution, because the same trigger exists here. On the rmx16 the pedal froze after 45–60 min of playing: LEDs held, no controls, no audio until a power cycle.

**Cause** (found on the rmx16): a stall on the AXI bus that no interrupt can break, triggered by the Cortex-M7 reading speculatively from the memory-mapped QSPI flash.
- The Cortex-M7 may read Normal memory speculatively, through whatever value a register holds (ES0392 2.1.2). libDaisy v8.1.0's MPU setup leaves the external-memory window 0x60000000–0xDFFFFFFF on the default map, where it is Normal memory; that includes the 8 MB memory-mapped QSPI region at 0x90000000.
- A repeated memory-mapped read of the last QSPI byte, 0x907FFFFF, stalls the AXI bus (ES0392 2.7.5, present on revision V).
- **The trigger here:** `Machine::cpuTicks` and `dspTicks` (`emu/src/machine.h`) count 30.72 MHz master ticks in 64 bits. Their low word passes 0x907FFFFF every 2³² / 30.72 MHz = 2.3 min `[INFERENCE: whether the compiled loop holds that word in a register was not traced]`.

**Fix.** `configureMpu()` (`src/mpu.h`) is called right after `hw.Init()` by both the pedal and the [engine bench](#engine-bench). It sets five regions; where they overlap, the higher number wins.

|Region|Range|Access|Memory type|Purpose|
|---|---|---|---|---|
|0|0x60000000–0xDFFFFFFF (subregions 3–6 of 4 GB)|none, execute-never|strongly ordered|ST's background region (AN4838; STM32CubeMX's default)|
|1|0x30000000, 32 KB|full|uncached|libDaisy's region 0: D2 SRAM1, the DMA buffers|
|2|0xC0000000, 64 MB|full|write-back|libDaisy's region 1: SDRAM|
|3|0x38800000, 4 KB|full|uncached|libDaisy's region 2: backup SRAM (`boot_info`, the restart record)|
|4|0x907F0000–0x907FBFFF (subregions 0–5 of 64 KB)|privileged read-only, execute-never|write-back|The [ROM store](#roms): slots 0–10 end at 0x907FAFFF and the [mono-output overrides](#editor-override) fill 0x907FB000–0x907FBFFF; `romSlotFlash()` and `loadOutMixStore()` read them memory-mapped|

- The CPU does not read a no-access region speculatively, and a real access to it faults and is recorded.
- 0x907FC000–0x907FFFFF, which holds the stalling byte, stays on region 0.
- Flash writes and erases use QSPI indirect mode through the peripheral's registers, not the window, so region 4 needs no write access.
- Region 4 keeps the default map's write-back type, so the cache invalidate in `romSlotFlash()` still applies.
- Nothing else in the window is read after boot. The app runs from SRAM, and the linker's QSPI output sections are empty.

**Recovery** (`src/crash_trace.h`, pedal only, copied from the rmx16) makes a future bug restart the pedal instead of freezing it, and records where it happened:
- **CPU faults.** HardFault, MemManage, BusFault and UsageFault go to a recorder that saves the fault, then resets. libDaisy's own handlers never return: HardFault ends in a breakpoint, and MemManage, which `HAL_MPU_Enable` turns on, is the startup file's `for (;;);` at priority 0.
- **Audio stalls.** The main loop resets if the audio callback completes no block for 100 ms. The callback keeps counting blocks during a program load, and a MODE ENH flip overruns one callback by about 4 ms.
- **Anything else.** An independent watchdog (4 s) resets if the main loop stops. Its longest block is a program load (0.8 s). The loop kicks the watchdog once per pass, and once per editor command, because a sector erase blocks it.
- **The record.** Kept in backup SRAM and printed as `event` lines ([USB log](#usb-log)). Inside HardFault the MPU is off and backup SRAM is cached, so the recorder cleans the D-cache before resetting.

**Checked on the device** (2026-10-03):
- A throwaway build read 0x907FFFFF from the main loop 15 s after boot. No event came before that. The read raised a MemManage fault, recorded as `cfsr 00000082 hfsr 00000000 mmfar 907FFFFF`. `addr2line` put the pc on the test line, and the pedal restarted and ran on.
- The final build read all eleven ROM slots through region 4 at boot (`roms BBBBBBBBBBB`, which needs each slot's byte sum) and loaded CONCERT HALL.
- The engine bench with the MPU printed its full report and reset into the bootloader.
- ROM flashing through the editor with the MPU on: all eleven slots flashed, erased and reverted with no `event` line ([ROMs](#roms)).
- Not yet run: hours of use without a restart (see [Open questions](#open-questions)).

## Engine bench

`daisy/bench/` is a separate firmware, `lexbench`, that measures and profiles the engine on the Seed without the codec. It replaces the pedal firmware while it is flashed.

```
make -C daisy/bench                                          # build/lexbench.bin
daisy/tools/flash_when_ready.sh daisy/bench/build/lexbench.bin
uv run daisy/tools/cdc_read.py 40 > bench.log
uv run daisy/bench/symbolize.py daisy/bench/build/lexbench.elf bench.log
```

The bench works through these steps:
1. It times each stage on CONCERT HALL.
2. It samples the program counter from TIM7 at about 20 kHz while CONCERT HALL runs through `Engine::process` for 5 s.
3. It boots all 22 programs and runs each for 1 s, on a synthetic sine.

It prints the whole report 4 times, 3 s apart, so a log reader has time to attach. It then resets into the Daisy bootloader (`System::ResetToBootloader`, infinite timeout). Once a host has read from the port, libDaisy's CDC print waits while no host reads, so the remaining reports and the reset only go ahead while `cdc_read.py` is attached (seen 2026-10-03). Reflash the pedal afterwards with `make -C daisy program-dfu`.

The bench sets up the same MPU regions as the pedal (`src/mpu.h`, see [Lockups](#lockups)), but has no crash recovery.

|Line|Content|
|---|---|
|`B …`|Loop length and DSP rate, then the permille of 480 MHz that 1 s of audio costs in each stage: input resampler, output resampler, `Machine::run`, the DSP loop alone, and the whole engine. Also the cycles per DSP step with DMEM in SDRAM and in AXI SRAM|
|`P name …`|Per program: boot time (ms), average and maximum load in permille of the 1 ms block period, and underruns|
|`S samples N other M`|Profiler totals: M samples fell outside the `.text` range|
|`H addr count`|One of the 60 busiest profile buckets (8 bytes of `.text` from `addr`)|

`symbolize.py` sums the `H` buckets per function, using `arm-none-eabi-nm` from `PATH` on the ELF, and prints each function's share of all samples.

`bench.cpp` includes `engine.h` with `private` and `protected` redefined as `public`, because it needs the engine's internal `Machine` and `Dsp`. That hack is limited to the bench.

## Open questions

- **Editor link on the device** (2026-10-02, from the host with `aseqsend`/`aseqdump`, since a running Chrome held the raw MIDI port). Confirmed: HELLO answered with STATE and every present page; the LOADING state, then the program, boot 749–761 ms; CONCERT HALL selected with the levers on bank 2 stayed loaded; LF DECAY 255 read back as 249; DYN DECAY, mix and bypass reflected within 20 ms; malformed frames ignored; turning the mix knob streamed mix updates every 20 ms. `cdc_read.py`'s MIDI path works (2026-10-03: the log lines above, across resets and reflashes). The browser editor ran against the device on 2026-10-03 in headless Chromium: LINKED, NO ROMS, the whole ROM panel ([ROMs](#roms)), the log panel and a program selection. Still to check: its faders against the device, the knob park after an editor move, and the MODE ENH lever override. While a slider was restored from 249 to 132, one report showed 240 before 132 `[unknown]`: probably the firmware's handler caught partway through.
- **Long run with the MPU.** Hours of playing and idle time without an `event` line would confirm that nothing legitimate touches the blocked window.
- **Mix knob taper.** The knob position maps linearly to the crossfade angle, so the overall level holds but the balance does not move evenly to the ear, which hears level roughly logarithmically. The wet-to-dry ratio is −16.0 dB at 10% of travel, −7.7 dB at 25%, 0 dB at 50%, +7.7 dB at 75% and +16.0 dB at 90%: most of the audible change happens near the ends. Not addressed yet.
