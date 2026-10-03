# Lexicon 224XL memory map

Sources:
- `224_service_manual.pdf`: Lexicon **Model 224** service manual, 1 July 80. It is a scan with no text layer, so its pages were read as images; page numbers cited below are 1-based PDF pages. It describes the original 224, not the 224X/XL. Anything that may differ on the XL is marked `[INFERENCE]`.
- Intel SBC 80/10 and SBC 80/10A Hardware Reference Manual (HRM): http://www.bitsavers.org/pdf/intel/iSBC/9800230F_iSBC_80-10_SBC_Hardware_Reference_Manual_Aug77.pdf
- Lexicon **Model 224X** service manual: https://therackofages.com/Lexicon/224x%20digital%20reverborator/Lexicon-224X-Service-Manual.pdf (scanned; PDF page numbers). Cited as "224X manual"; a bare "manual" means the Model 224 manual.
- ROM images in `roms/224XL_v8_21/` (see [rom_map.md](rom_map.md)).

To regenerate the RAM and I/O port cross-reference used below, run:

```
uv run tools/ram_xref.py
```

The script sweeps the known code regions linearly and lists every direct RAM reference (LDA/STA/LHLD/SHLD/LXI) and every IN/OUT. Inline data inside a region can produce false hits. For the ROM tables, run `uv run tools/rom_map.py`.

## 1. CPU address space (8080, 64 KiB)

|Range|Device|Evidence|
|---|---|---|
|0000–07FF|SBC ROM 1, socket U23 (SBC1)|See below|
|0800–0FFF|SBC ROM 2, socket U25 (SBC2)|See below|
|1000–17FF|SBC ROM 3, socket U24 (SBC3)|See below|
|1800–1FFF|SBC ROM 4, socket U26; empty in this set|See below|
|2000–27FF|NVS card battery-backed RAM, 2 KiB. The NVS card is XL-era and is not in the 1980 manual|SBC2 RAM test at 0864 (`LXI H,2000h` at 0867, `LXI H,27FFh` at 087F). SBC1 0633–06EC walks 2000/2200/2400/2600 in 200h blocks `[INFERENCE: the "0 NVS INIT" diagnostic]`|
|2800–3BFF|No direct references found `[INFERENCE: unmapped]`|`tools/ram_xref.py`|
|3C00–3FFF|SBC on-board RAM, 1 KiB|Manual p.48. The manual p.22 RAM test reports by 256-byte page: E10 = 3C00–3CFF, E11 = 3D00–3DFF, E12 = 3E00–3EFF, E13 = 3F00–3FFF. The stack starts at 4000h (`LXI SP,4000h` at SBC1 0075, 009A, 00B5, 02FF, 034D) and grows down into this RAM|
|4000–41FF|T&C writable control store (DSP program), 128 steps × 4 bytes|224 manual p.23: byte lanes +0 = U49, +1 = U33, +2 = U18, +3 = U3 (diagnostic errors E20–E23) on the 224 T&C. On the 224X T&C (224X manual p.111) the lanes are U43, U29, U15, U2, and step n is WCS address 127 − n; see [dsp_microcode.md](dsp_microcode.md)|
|4200–7FFF|No direct references found `[INFERENCE: unmapped]`|`tools/ram_xref.py`|
|8000–FFFF|NVS EPROMs NVS1–NVS8, 8 × 4 KiB|See [rom_map.md](rom_map.md)|

Evidence for the SBC ROM rows:
- Manual p.48: the SBC is a National BLC-11 or Intel SBC 80/10A, with four 2716s at 0–1FFF.
- Manual p.21: ROM 1 = U23, ROM 2 = U25, ROM 3 = U24, ROM 4 = U26.
- Manual p.51: install order is [23][25][24][26].
- The SBC1 checksum loop at 0700 expects each 2 KiB block's sum to equal its ROM number.

## 2. SBC RAM 3C00–3FFF

Confirmed variables, from the loader disassembly at NVS3 AA00–AA70:

|Addr|Use|
|---|---|
|3E01|Pointer to the active microcode record. SHLD at AA02. AA6A reloads it and adds C0AAh (= +AAh −4000h, the step area mapped onto the DSP RAM offset)|
|3E03|3E01 + 4 (SHLD AA09)|
|3DF8|Record bytes 01–02 (SHLD AA12): the page-presence mask, where a set bit 16 − *p* skips page *p* in the builder walk (A7AE–A7BC, A877). 3DF6 is its shifted working copy|
|3E4E–3F4D|Per-step 2-byte image, 128 steps. Copied from the top down into DSP RAM bytes +1/+0 of each step (loop AA38–AA4B: `LXI H,3F4Dh` / `LXI D,41FDh` / `MVI B,80h`). After the copy, the loader reuses the area from 3E4E up for the step-group entries (AA99; see 3CF4)|
|3DFD / 3DFF|Save the DSP RAM words at 4138h / 4074h (AA4E–AA57) before AA5A–AA63 overwrites them with E57Fh / EA77h|

Parameter and live-control variables, from disassembly and differential emulator runs (details and evidence in [parameters.md](parameters.md)):

|Addr|Use|
|---|---|
|3CA2–3CD1|Program register: the microcode ID and 47 parameter bytes, copied from preset block bytes +04…+33 at boot (SBC1 0614). The slider at storage page *s*, slider *k* is 3CA3 + 6*s* + *k* (NVS1 862E)|
|3C73–3CA1|Working copy of 3CA3–3CD1 (A91B, AA7C)|
|3CCD|Option bits: 01 DYN DECAY, 40 MODE ENH, 80 DECAY OPT|
|3CCE / 3CCF|SIZE / SIZE 2. Read by the offset-image builder (B574, B5FC) and the reload check (A9C2, A9D2), against the working copies 3C9E / 3C9F|
|3CD0|REV STOP DLY, the DYN DECAY hold time (copied to 3C5F)|
|3C30 / 3C32 / 3C33 / 3C34|Current page-entry pointer / page index / page number / storage-page index (SBC3 144F–14A3)|
|3C51|DYN DECAY state: bit 0 armed, bits 7/6 MID/LF ramp active|
|3C52 / 3C53|Active LF / MID decay values used by the decay rebuild A7A9|
|3C50 / 3C54 / 3C5F|Smoothed input level code / held peak level / DYN DECAY hold counter|
|3C63 / 3C6A|OR-accumulated headroom bits from IN 08 / IN 09 (SBC2 0FA2)|
|3E38|Peak \|X register\| since the last level conversion (NVS1 82A0–82C0)|
|3E3D|Level code if ≥ BCh, else 10h (82FF–830A). The builders also use it as scratch|
|3E0B|DEFINITION limit ((FF − value) >> 3) + 1 (B286); A8F8 limits s(MID) to 3E0B − 1|
|3E3E / 3E3F|Channel-1 / channel-2 input of the group-1 allpass builder B000 (A805, B290–B299)|
|3E40 / 3E42|New / current PREDELAY delay; 3E07 bit 0 requests the ramped offset change (B2DD–B301, B315–B388)|
|3C55–3C58|The four DEPTH gains from B193|
|3E10–3E16|Peak-level code and its history, used by DECAY OPT (830D–8404). 3E12 is the DECAY OPT reduction of the group-1 allpass g (84E4–8515, 822A, 833E)|
|3CD2 / 3CD4|MODE ENH update divider / step size, set from CHORUS (B3BA–B3DD)|
|3CD3|MODE ENH segment length: preset +33 (record byte 30h = FE) or +31 (FF) (A8A8–A8CB)|
|3CD5|MODE ENH excursion window mask: record byte 35h (FE) or preset +33 (FF) (AA1F, A8C8; read at AE02)|
|3CD8–3CE4|SIZE-map parameters, record bytes 36h–42h (loader AA23): T0, T1, T2, T3 (words), then f, lo, hi, lo2, hi2 (B55B, B683; [parameters.md](parameters.md) §5.2). Meaningful only in FE records; B65A sets 3CDC = FFFFh for FF records, which disables the map|
|3CE5 / 3CE7|SIZE-map anchors: S(T1) + 1, and the midpoint between it and 10000h (B58F–B5A3)|
|3CE9 / 3CEA|Stretch factors in 1/16 for the main / lower band (B60C, B5B3–B5F8); 10h = ×1|
|3CEB / 3CF1|Image-builder pointers: record step pointer (from record + 2A8h) and 3E4E–3F4D image write pointer (B627–B659)|
|3CF4–3DF2|Step-group table, 3 bytes per group: `[entry pointer lo][hi][header]` (group 0 = MODE ENH taps, so 3CF6 is their count/flags). Built at load (AA81–AB38) from the record's group list; cleared at A9ED|
|3E44–3E49|Mode-enhancement timers and the pseudo-random pointer: 3E44 (from 3CD2) gates the updates; 3E45 (8 updates) × 3E46 (from 3CD3) sets the segment length; 3E47 walks 8000–8FFF, and the ROM byte there goes to 3E49, whose bit 0 is the tap direction (AD5C–AD99)|

Directly referenced addresses per page:

|Page|Addresses|
|---|---|
|3C|126|
|3D|12|
|3E|32|
|3F|40|
|Total|210|

All other addresses are `[unknown]`; `tools/ram_xref.py` lists every one with its code sites.

## 3. NVS RAM 2000–27FF

```
== NVS RAM 2000-27FF: 14 addresses
2000  LDA 086E 0886; LXI D 0650 0E5D; LXI H 0633 0641 0867 0E69 0F7F
2001  LXI D 0F82 1334 AC12 AC68; LXI H 8149 AC54 ACE3
2031  LDA 12ED 131F 9098; STA 13C9 9395 9403
2032  LDA 130B 90C6 910E; STA 13C4 908F 90DB; LXI H 90B1
2033  LDA 12FF 9357; STA 815A; SHLD 939B 93FE
2034  LDA 1305 935E; STA 815D
2035  LXI H 803E 8088 95FB 96E0
2067  LXI H 0F92 8001 8019 8030
2068  LXI D 0F95
20AF  LXI D 80BC; LXI H 85A9
2200  LXI D 0684; LXI H 0667 0675
2400  LXI D 06B8; LXI H 069B 06A9
2600  LXI D 06EC; LXI H 06CF 06DD
27FF  LXI H 087F
```

- SBC3 12ED–1334 are accessor routines for 2031–2034.
- 2001–2030 hold a saved copy of the program register 3CA2–3CD1 (SBC3 1330: `LXI H,3CA2h`, `LXI D,2001h`, 30h bytes), written after option toggles.
- The rest of the NVS RAM layout is `[unknown]` `[INFERENCE: user registers/patches, per the NVS card description]`.

## 4. DSP program memory 4000–41FF

- Step n occupies 4000h+4n … 4000h+4n+3, as a little-endian 32-bit microinstruction. The T&C uses the active-low bus lines directly, so step n is WCS address 127 − n and the stored bits are complemented; see [dsp_microcode.md](dsp_microcode.md) for the field layout.
- Bytes +2/+3 (op, registers, sign, `XFER`, `ZERO`, coefficient) are copied from the record step area (record+AAh; loader AB55–AB6E). Bytes +0/+1 come from the 3E4E–3F4D image (AA38). They are the 16-bit `OFST/` data-memory offset, or the I/O sub-operation bits in I/O steps (224X manual p.18; T&C schematic, p.111).
- Load sequence, from AA2E–AA68 (the same pattern appears at AB39/AB41 and ACEE–ACF4):
  1. `OUT 00h` (single-cycle mode), then `OUT 02h` (halt immediately);
  2. write the DSP RAM;
  3. `OUT 01h` (continuous-run mode), then `OUT 03h` (continue).

```
== DSP program RAM 4000-41FF: 25 addresses
4000  LXI H 08F9 0E95 0F2C; LXI SP 0075 009A 00B5 02FF 034D
4001  LXI D 0E98 0F2F
4003  LXI D AB16
4072  SHLD ACFC
4074  STA 0F56; LHLD AA54; SHLD AA63 AB4C AD51
4076  STA 0F51; SHLD AD45
407C  STA 0EBF
407E  STA 0EBA
4130  STA 0F0F
4131  STA 0F14
4135  LXI H AD2D
4137  LXI D 0EAD 0F47; LXI H AB77
4138  LHLD AA4E; SHLD AA5D AB46 AD4B
413A  SHLD AD42
413E  LDA B0D4
4180  LXI D 0D04
41F5  STA 0B8D 0BAD
41F8  STA 0F05
41F9  STA 0F0A; LXI H AD1B
41FA  STA 0957 0964 0976 0D3D 0D4C 0D5B 0DC4; LXI D 0C7B
41FB  STA 0969 0983 0DB7 0DC9 0DD6 0DE3; LXI H AD06
41FC  STA 0AA4
41FD  STA 0AFF; LXI D AA3B
41FE  STA 0B17; SHLD 0B3B ACF9
41FF  STA 0B29; LXI D 0935 AB5C; LXI H AB71
```

Most of these sites are the SBC2 tests (0864–0FFF). The `LXI SP` sites at 4000 are the stack setup (§1), not DSP RAM accesses.

## 5. DSP data memory (DMEM): not mapped into the 8080 address space

From manual pp.36, 39:
- Absolute address = current-position counter − offset. The 14-bit counter (U28, U90) increments once per sample period. The offset comes from the microinstruction as the complemented `OFST/` and is added with the carry-in high. On the 224 this gives 16 K × 16-bit words across 16 dynamic RAMs, refreshed during non-memory microinstruction cycles.
- The 8080's only data path to the DSP is the X registers: OUT 06h/07h write the one the DSP reads (`RD XREG`), and IN 06h/07h read the one the DSP writes (`WR XREG`). They are separate registers: the self-tests SBC2 0A57–0A60 and 0C48 fail otherwise ([aru_fpc.md](aru_fpc.md) §3).
- OUT 05h clears the current-position counter.

224X manual p.20 (§3.6): the 224X DMEM has a 16-bit current-position counter (U51, U65) and a 16-bit `OFST/`, with either one bank of 64 K or two banks of 16 K dynamic RAMs. A stock 224XL has 32 K words (two 16 K banks), upgradable to 64 K.

**V8.2 requires 64 K words.**
- **Power-on DMEM test** (SBC2 0B75–0C27, reached on every power-up after the LARC handshake: 0089 → ROM checksums 0703 → NVS RAM 0864 → SBC RAM 0897 → WCS 08F9 → … → `CALL 0B75` at 0928 → 00CC). Each `OUT 03` in single-cycle mode is taken to run one pass of the test program `[INFERENCE]`; `Machine` models it that way, and the emulated test passes (`emu/tests/selftest_test.cpp`). The steps:
  1. It loads an 8-step test program (0C28, via 0933) that each pass:
     - writes X to DMEM[pos − offset(step 125)];
     - loads X with DMEM[pos];
  2. **Alias probe** (0B7F–0BA9):
     - write 5555h at address 0;
     - set the step-125 offset to 8000h (`STA 41F5`) and write AAAAh;
     - read address 0 back.

     On a 32 K card the read gives AAh. The intended result is 00 → 256 pages (64 K) for 5555h, or 80h → 128 pages (32 K) otherwise, stored in 3E3D. But 0BA4 is `JMP 0BA9`, not a conditional jump, so 3E3D is always 00 and `MVI A,80` at 0BA7 is unreachable.
  3. It fills all 65536 words with 5555h + *i* (0BB4–0BCD).
  4. It verifies them, rewriting with AAAAh + *i* (0BD0–0C04), and checks again (0C05).
  5. On a 32 K card the second half of the fill overwrites the first, so the verify fails at word 0 in the high byte (code 02, the `IN 07` compare at 0BEF–0BF2). 092D then reports it through the fatal-error display 021D with type 90h. That routine waits for a LARC key before the boot continues.
- **History.**
  - 224X v8.1 (SBC1 0577–05CC) runs the same fill/verify over exactly 32 K words (`CPI 80h`), with no probe.
  - 224XL v8.1A already has the probe with the same `JMP` (SBC2 09EE–0A0C, `JMP` at 0A07).

  The XL firmware therefore sized the test for 64 K and turned the 32 K fallback off.
- **Programs.** Booted in the emulator, 14 of the 22 factory programs use memory-op offsets spanning more than 32768 words, with the default presets. Examples: CONCERT HALL 0000–A8C3h, RICH CHAMBER up to C07Fh, RICH SPLIT up to D245h. On a 32 K card those delays would alias, i.e. change by 32768 samples.
  - CD PLATE A's only high offset is FFFFh, which is −1 on either size.
  - The SIZE map's anchors M and [3CE7] are midpoints computed modulo 10000h (B595–B5A3, B6B9–B6C8), and the PREDELAY clip is FFF6h − [3CE7] (§2 of [parameters.md](parameters.md)). At SIZE FE, RICH CHAMBER's main band reaches FEBEh.

## 6. I/O ports

Ports 00h–09h are decoded on the DMEM card (manual p.39: U45 decodes OUT 0–7; U46/U47 decode IN 0–9). Function text comes from the manual p.28 port table (224 T&C/DMEM); the XL loader's use of them is consistent with it. ROM sites are the `== I/O ports` output of `tools/ram_xref.py`.

|Port|Dir|Function|ROM sites|
|---|---|---|---|
|00|OUT|Single-cycle mode|SBC2 0A44 0A63 0A7E 0B85 0BB0 0C48 0D20 0D9B; NVS3 AA2E AB39 ACEE|
|01|OUT|Continuous-run mode|SBC2 0A6D 0D0C 0EC2 0F59; NVS3 AA66 ABD3 ACF4|
|02|OUT|Halt immediately (also issued at reset, 003B)|SBC1 003B 0052 034A 0631 0706; SBC2 089F 0933 0A57 0A82 0B5C 0B75 0C4A 0CFF 0D1E 0D99 0E93; NVS3 AA36 AB41 ACF2|
|03|OUT|Continue|SBC2 09A7 0A46 0A4A 0A6F 0A80 0B89 0B98 0B9C 0BC0 0BE7 0C89 0C96 0D0E 0D22 0D39 0D48 0D57 0D66 0D9D 0DFE 0EC4 0F5B; NVS3 AA68 ABD5 AD54|
|04|OUT|No function listed|None|
|05|OUT|Clear current-position counter|SBC2 0B87 0B96 0B9A 0BB2 0BD6 0C0B|
|06|OUT|Write X-in register low byte (read by the DSP's `RD XREG`)|SBC2 09A4 0AA7 0AD2 0B81 0B92 0BBB 0BE2 0C85 0D32 0D41 0D50 0D5F 0DF7|
|07|OUT|Write X-in register high byte / bus test register|SBC2 09A0 0A20 0A2D 0A42 0A59 0A74 0AA9 0AD4 0B83 0B94 0BBE 0BE5 0C87 0D37 0D46 0D55 0D64 0DFB|
|00|IN|OFST0–7|SBC2 0A8F 0AAB|
|01|IN|OFST8–13 in bits 0–5, bit 6 XFER, bit 7 CAS|SBC2 0AF5 0B03|
|02|IN|Read bus test register|SBC2 0A22 0A2F|
|03|IN|Bits 0–7 = `SAT/`, `AS0`, `AS2`, `AS1`, `C3⊕C2`, `C1⊕C0`, `S5⊕C4`, `ZERO`. The AS0–AS2 cells also carry `S0⊕S1` terms with overbars (handwritten; overbars uncertain)|SBC2 0991 0AC5 0B2E 0FF3|
|04|IN|`WA0 WA1 RA0 RA1 CSIGN RD AD MEM W RESET/`|SBC2 0AE9 0B1D|
|05|IN|`SDAA/ SDAB/ SDAC/ SDAD/ WR DA WR XREG RD XREG RD RREG`|SBC2 0A97 0B0D|
|06|IN|X-out register low byte (written by the DSP's `WR XREG`)|SBC2 09B5 0AB3 0AD6 0B9E 0BE9 0C99 0E01; NVS1 82AE|
|07|IN|X-out register high byte|SBC2 09AA 0A4C 0A5B 0A65 0A76 0ABB 0ADE 0BEF 0CA1 0E0B; NVS1 82A0|
|08|IN|CH1 headroom: 5-bit peak-hold thermometer code, bits 0–4 = the 5.0/2.24/1.12/0.56/0.28 V comparators (≈ 0/−7/−13/−19/−25 dB) `[INFERENCE: order]`; the read presets the register (see Notes)|SBC2 0FA2; NVS1 81D4|
|09|IN|CH2 headroom, as IN 08|SBC2 0FAC; NVS1 81D9|
|EE|IN|8251 USART data|SBC1 0069 0082 0413; SBC3 1245 1248; NVS2 97C8|
|EE|OUT|8251 USART data|SBC1 0067 0513 0560 057C 05B0; SBC3 123B; NVS2 981E|
|EF|IN|8251 USART status|SBC1 006B 007B 03F2; SBC3 1233 123E 124A; NVS2 97C1|
|EF|OUT|8251 USART control|SBC1 004C 0050 0057 040E 0505 0592 05A9; NVS2 9483 98F2|

Notes:
- The application reads the headroom ports IN 08/09 at 81D4/81D9, and through SBC2 0FA2 in the audio-time level detector (NVS1 82CF).
  - Per the 224 manual (FPC "Headroom", p.44; AIN "Gain Ranger", p.55) and the 224X manual (Table 3.3, PDF p.28), five AIN comparators feed a per-channel 5-bit peak-hold register. On the 224X they sit at 5.0/2.24/1.12/0.56/0.28 V of a 5 V clip, about 0/−7/−13/−19/−25 dB; only the middle three set the gain, and the outer two serve the display only. Reading the register (HR1/ or HR2/) presets it again ([aru_fpc.md](aru_fpc.md) §4).
  - When the headroom bits are zero, the detector falls back to the X-register peak, but only below 0800h (−24 dBFS); a larger peak is replaced by 000Ah (82EC–82F5), which cannot occur on hardware.
  - `lex224emu` models the ports with `fpcHeadroom()` and per-channel latches cleared on read (see [parameters.md](parameters.md) §4.1).
- EE/EF: the HRM says the 8251 is selected for EC–EF, with data at EC/EE and control/status at ED/EF. The XL code uses EE/EF only: init at 004A (first `OUT EFh` at 004C), the RST 7 handler at 03EE, plus SBC3 1233–124A and NVS2 97C1–98F2.
- E4–E7 / E8–EB: SBC 8255 PPIs #1/#2 (HRM). The manual p.48 describes the 224 remote head on 8255 port A (mode 2) and port B (mode 0). The XL firmware touches E4–E7 only in the dead SBC2 0800–0863 fragment (not swept by `tools/ram_xref.py`) `[INFERENCE: the LARC talks over the 8251 serial link]`.

## 7. Open questions

- The display text of the DMEM error: the type is built from D = 90h plus the failure code (0217/021D).
- Whether 224 port semantics hold unchanged on the 224X T&C.
- The meaning of every SBC/NVS RAM variable not listed in §2.
- The NVS RAM layout.
- The LARC link protocol over EE/EF.
- The function of OUT 04.
- Possible false hits from the linear sweep.
