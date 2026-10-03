# Lexicon 224XL v8.2 ROM contents map

Source images: `roms/224XL_v8_21/`. The version string in SBC1 reads `224XL V8.2`, even though the folder is named v8_21.

The service manual PDF (`224_service_manual.pdf`) is a scanned image with no text layer. Everything below was derived from the ROM bytes. Items marked `[INFERENCE]` or `[unknown]` are not confirmed.

To regenerate and cross-check the checksum, record-slot, preset and factory-program tables, run:

```
uv run tools/rom_map.py
```

The script exits 1 if a checksum fails or a factory program does not resolve to a microcode record.

## 1. Summary

The 8-bit byte sum of each ROM equals its socket index. This confirms both the identity and the order of the images.

|File|Size|CPU address|8-bit sum|Contents|
|---|---|---|---|---|
|`SBC1 2716.BIN`|2 KiB|0000–07FF|01|Reset, USART init, diagnostics menu, ROM checksum test|
|`SBC2 2716.BIN`|2 KiB|0800–0FFF|02|Hardware tests (NVS RAM, DSP program RAM, DSP ports)|
|`SBC3 2716.BIN`|2 KiB|1000–17FF|03|Shared library: label/record/preset/program lookups|
|`NVS1 2732.BIN`|4 KiB|8000–8FFF|01|Main application code (entry 813B)|
|`NVS2 2732.BIN`|4 KiB|9000–9FFF|02|Code, label pointers, label table 1 (start)|
|`NVS3 2732.BIN`|4 KiB|A000–AFFF|03|Label tables, factory program table, slider layouts, DSP loader code|
|`NVS4 2732.BIN`|4 KiB|B000–BFFF|04|Code; microcode records #0–#2|
|`NVS5 2732.BIN`|4 KiB|C000–CFFF|05|Microcode records #3–#8|
|`NVS6 2732.BIN`|4 KiB|D000–DFFF|06|Microcode records #9–#14|
|`NVS7 2732.BIN`|4 KiB|E000–EFFF|07|Microcode records #15–#19 (#20 empty)|
|`NVS8 2732.BIN`|4 KiB|F000–FFFF|08|24 per-program preset blocks|

Evidence for the address mapping:
- The ROM checksum loop in SBC1 at 0700–0768 sums each 2 KiB SBC block and expects the block index: blocks 1–3, and the constant 3E3Dh=4. It then repeats from `LXI H,8000h` over 4 KiB blocks, expecting indices 1–8.
- Jump targets in each ROM fall inside that ROM's own address range.
- The 12-character label table runs without a break from 9FFF into A000.
- The fourth SBC socket (1800–1FFF) is empty.

## 2. CPU memory map and I/O

Moved to [memory_map.md](memory_map.md).

## 3. Per-ROM regions

### SBC1 (0000–07FF): boot, diagnostics, checksum
|Range|Contents|
|---|---|
|0000|`JMP 003Bh`|
|0003|Byte 11h `[INFERENCE: checksum-adjust byte]`|
|0004–0037|FF|
|0038|RST 7 → `JMP 03EEh` (USART interrupt)|
|003B–00D7|Reset/USART init. At 00CC: `LDA B800h`; if ≠0, `JMP 813Bh` (NVS1 application), otherwise diagnostics at 0089|
|00D8–0219|Diagnostic strings: `224XL V8.2  DIAGNOSTICS`, `DIAG RESTART`, `DIAG ERROR TYPE…`, test menu `0 NVS INIT … 9 TEST ALL`|
|021A–0768|Diagnostic/utility code. ROM checksum test at 0700–0768|
|0769–07FF|FF|

### SBC2 (0800–0FFF): hardware tests
|Range|Contents|
|---|---|
|0800–0863|Fragment assembled for origin 0: jumps to 001C/0026/0035/005D, drives ports E4h–E7h, and is never referenced `[INFERENCE: dead leftover test code]`|
|0864|NVS RAM test (2000–27FF)|
|08F9|DSP program RAM test (4000–41FF), then the DSP tests 0A1D (bus, single cycle), 0C48 (ARU register file) and 0942 (ARU multiplier), decoded in [aru_fpc.md](aru_fpc.md) §3, and the DMEM test (`CALL 0B75` at 0928); ends with `JMP 00CC`|
|0991–0FFF|DSP hardware tests on ports 00h–09h (ARU/FPC signature tests, per the SBC1 menu) `[INFERENCE for per-routine mapping]`|
|0B75–0C27|DMEM test: a disabled 32 K/64 K alias probe (the `JMP` at 0BA4), then a 64 K-word fill/verify. Test program at 0C28. See [memory_map.md](memory_map.md) §5|

No FF fill.

### SBC3 (1000–17FF): shared library used by NVS code
|Addr|Contents|
|---|---|
|1000–102A|Code. 1020 indexes a 32-entry table of 2-byte words (`ANI 1Fh`, ×2).|
|102B–10AA|Two 32-entry word tables at 102B and 106B, read through 1020 (from 10D1 and 1118). Purpose `[unknown]`|
|12B7|Label lookup: HL = word at `98F6h + 2·(C−1)`, plus 12·A|
|12ED–1334|Accessors for NVS RAM 2031h–2034h|
|133E|Microcode record lookup (walk at 1343–135E, see §4)|
|1361|Preset-block lookup at F000 (walk at 1361–1378)|
|1398|Factory program table lookup at A446|
|16B9–17FF|FF|

### NVS1 (8000–8FFF): main application code
Entry point 813B. All 4 KiB is code; no FF fill.

### NVS2 (9000–9FFF): code + labels
|Range|Contents|
|---|---|
|9000–98F5|Code|
|98F6–98F9|Label-table pointers: 98FAh (table 1), A1B2h (table 2)|
|98FA–9FFF|Start of label table 1 (continues to A1B1)|

### NVS3 (A000–AFFF): labels, program tables, DSP loader code
|Range|Contents|
|---|---|
|A000–A1B1|End of label table 1, including program names from index 96h (`CONCERT HALL` at A002)|
|A1B2–A445|Label table 2: 55 × 12 chars (units `SEC MSEC HZ KHZ % METERS`, UI and tape prompts)|
|A446–A4B4|Factory program table: 22 × 5-byte entries + FF terminator|
|A4B5–A4D2|15 little-endian pointers to slider-page lists, indexed by the program's layout byte. Pointer 0 (A4D3) is the master list of pages 1–13; pointer *L* is layout *L*'s override list (see [parameters.md](parameters.md) §2)|
|A4D3–A756|Page lists: 10-byte entries `[page #][title label][6 slider labels][flags][count]`, each list FF-terminated|
|A757–A760|FF, then the option-toggle table: 3 × `[table-2 label][bit mask in 3CCD][action]` for DYN DECAY, MODE ENH and DECAY OPT (NVS2 917A–91FE)|
|A761–A78B|Character set for label entry (`-*&/.0987654321ZYX…A $`)|
|A78C–A790|5 bytes `1C 19 15 10 29` `[unknown]`|
|A791–AFFF|Code, including the DSP program loaders (below)|

Loader locations:
- AA02 stores the record pointer to 3E01h.
- AB39–AB6E copies record microcode into 4000–41FF.
- AA2E and ACE0–AD54 initialise/patch DSP RAM.

### NVS4 (B000–BFFF)
|Range|Contents|
|---|---|
|B000–B71A|Code|
|B71B–B7FF|FF|
|B800–BFFD|Microcode record slots #0–#2|

### NVS5 (C000–CFFF)
Microcode record slots #3–#8.

### NVS6 (D000–DFFF)
Microcode record slots #9–#14.

### NVS7 (E000–EFFF)
Microcode record slots #15–#19. Slot #20 (ED54–EFFD) is empty (all FF).

### NVS8 (F000–FFFF)
|Range|Contents|
|---|---|
|F000–F96D|24 per-program preset blocks|
|F96E–FFFD|FF|
|FFFE–FFFF|DA FF `[INFERENCE: checksum-adjust byte at FFFE]`|

### Record-area gaps
The last 2 bytes of each 2 KiB block from BFFE to EFFE belong to no record:

|Addr|Bytes|
|---|---|
|BFFE|12 69|
|C7FE|12 65|
|CFFE|10 0B|
|D7FE|11 2E|
|DFFE|00 B2|
|E7FE|C6 31|
|EFFE|FF 3E|

Purpose `[unknown]`.

## 4. Data structures

### Microcode record table (SBC3 133E)
- 21 slots starting at B800h, each 0x2AA bytes. After each `+2AAh`, if the low address byte is FEh, add 2 more (`CPI FEh` at 1343–135E). This gives 3 slots per 2 KiB block.
- Byte 0 of a slot is the microcode ID (FF = empty).
- Bytes 00–A9 are a 170-byte header (parameter/patch data; the format is mostly `[unknown]`). Bytes AA–2A9 hold 128 DSP steps × 4 bytes. Known header bytes:
  - 01–02: the page-presence mask, which the loader keeps at 3DF8 (AA12). See [parameters.md](parameters.md) §2, "Builder walk";
  - 05h–33h: one descriptor byte per parameter, at the same offset as its preset byte. For delay parameters, bits 7–5 set the step unit (B41F);
  - 30h: FE or FF. It selects the MODE ENH parameter bytes (A8A8–A8CB): with FE, preset +33 is the segment length; with FF, +31 is the segment length and +33 the window mask. See [parameters.md](parameters.md) §5.3;
  - 31h: FE in RICH SPLIT only. The reload check (A9CD) then also compares SIZE 2;
  - 34h → 3C5B, and 35h → 3CD5 (loader AA19–AA1F). In FE records 35h is the MODE ENH window mask. In FF records it is the first byte of the step-group list, and A8C8 then overwrites 3CD5 with preset +33;
  - 36h–42h (FE records only) → 3CD8–3CE4 (loader AA23): the SIZE map, i.e. the band edges T0, T1, T2 and T3, then f, lo, hi, lo2 and hi2. See [parameters.md](parameters.md) §5.2. In FF records these bytes are step-group-list data, and the map is disabled (B65A);
  - 44h (FE) or 35h (FF) onward: the step-group list, groups of `[header][entries]` ending at FF, which the loader (AA81–AB38) turns into the RAM table at 3CF4 (see [parameters.md](parameters.md) §2).
- The loader at AB55–AB6E copies step bytes +2,+3 of step n into DSP RAM at 4000h+4n+2,+3. Bytes +0,+1 (the delay offset, or the I/O sub-operation bits) come from an 8080 RAM image at 3E4E–3F4D. B55B builds it (`CALL` at AA2B, before the halt at AA2E) from the record's own bytes +0,+1: I/O steps are copied, and memory-op offsets pass through the SIZE map ([parameters.md](parameters.md) §5.2). The loop at AA38 then copies it into the WCS. The field layout and a decoder (`tools/ucode_dump.py`) are in [dsp_microcode.md](dsp_microcode.md).

|#|Addr|ROM|ID|
|---|---|---|---|
|0|B800|NVS4|01|
|1|BAAA|NVS4|02|
|2|BD54|NVS4|81|
|3|C000|NVS5|08|
|4|C2AA|NVS5|10|
|5|C554|NVS5|20|
|6|C800|NVS5|40|
|7|CAAA|NVS5|05|
|8|CD54|NVS5|21|
|9|D000|NVS6|06|
|10|D2AA|NVS6|0A|
|11|D554|NVS6|12|
|12|D800|NVS6|0C|
|13|DAAA|NVS6|14|
|14|DD54|NVS6|24|
|15|E000|NVS7|22|
|16|E2AA|NVS7|11|
|17|E554|NVS7|80|
|18|E800|NVS7|42|
|19|EAAA|NVS7|41|
|20|ED54|NVS7|FF (empty)|

### Preset blocks (SBC3 1361)
- A chain starting at F000: `[program id][len lo][len hi][microcode id][…len−1 more bytes]`.
- Next entry = entry + 2 + len. The chain ends on an FF program id.
- Bytes +04…+33 are copied at boot into the program register 3CA2–3CD1: +04 is the microcode ID, and +05…+33 are the slider and option values (+2F holds the option bits). The per-byte meaning for CONCERT HALL is in [parameters.md](parameters.md) §3. Bytes +35 onward changed nothing at boot `[unknown]`.
- 24 entries:

|Addr|Program id|Len|Microcode|
|---|---|---|---|
|F000|01|008F|01|
|F091|44|0033|44|
|F0C6|05|008F|05|
|F157|80|00DA|80|
|F233|41|00E9|41|
|F31E|11|00CF|11|
|F3EF|81|0047|81|
|F438|02|0077|02|
|F4B1|03|0082|02|
|F535|08|0033|08|
|F56A|20|0085|20|
|F5F1|04|005C|01|
|F64F|40|005D|40|
|F6AE|09|0033|09|
|F6E3|10|0049|10|
|F72E|21|0033|21|
|F763|24|0033|24|
|F798|14|0033|14|
|F7CD|0C|0082|0C|
|F851|22|0033|22|
|F886|06|0033|06|
|F8BB|42|0033|42|
|F8F0|0A|0048|0A|
|F93A|12|0032|12|

Program ids 44 and 09 have no factory-program entry, and there are no microcode records 44 or 09. They are orphans `[INFERENCE: leftovers]`.

### Factory program table (A446, SBC3 1398)
- 5-byte entries: `[program id][bank 1–5][program #][label idx (table 1)][layout idx]`. The table ends on FF.
- Resolution chain: program id → preset block → microcode id → record slot.
- Two programs share microcode with another program:
  - ROOM (04) uses CONCERT HALL's microcode 01.
  - SMALL PLATE (03) uses PLATE's microcode 02.
- The table has no output-routing field. The original 224 manual lists a per-program mono (MIMO) output, A or A + C, but V8.2 has no equivalent: the fifth byte is the slider-page index (pointer table A4B5), and the only ROM text containing "OUTPUT" is `OUTPUT MUTE ACTIVATED` (A296).

Rows are in bank/program order below. In ROM, B3 P5, B2 P5, B2 P6 and B5 P5 come last.

|Bank|Program|Name|Program id|Layout|Microcode|Slot|Record|
|---|---|---|---|---|---|---|---|
|B1|P1|CONCERT HALL|01|1|01|#0|B800 NVS4|
|B1|P2|BRIGHT HALL|20|1|20|#5|C554 NVS5|
|B1|P3|DARK HALL|05|4|05|#7|CAAA NVS5|
|B2|P1|ROOM|04|1|01|#0|B800 NVS4|
|B2|P2|SMALL ROOM|40|4|40|#6|C800 NVS5|
|B2|P3|CHAMBER|08|3|08|#3|C000 NVS5|
|B2|P4|RICH CHAMBER|80|C|80|#17|E554 NVS7|
|B2|P5|DARK CHAMBER|41|C|41|#19|EAAA NVS7|
|B2|P6|INVERSE ROOM|81|D|81|#2|BD54 NVS4|
|B3|P1|PLATE|02|2|02|#1|BAAA NVS4|
|B3|P2|SMALL PLATE|03|2|02|#1|BAAA NVS4|
|B3|P3|CD PLATE A|21|3|21|#8|CD54 NVS5|
|B3|P4|CD PLATE B|10|3|10|#4|C2AA NVS5|
|B3|P5|RICH PLATE|11|C|11|#16|E2AA NVS7|
|B4|P1|CHORUS&ECHO|0C|9|0C|#12|D800 NVS6|
|B4|P2|RES CHORDS|14|A|14|#13|DAAA NVS6|
|B4|P3|M BAND DELAY|24|B|24|#14|DD54 NVS6|
|B5|P1|HALL/HALL|06|5|06|#9|D000 NVS6|
|B5|P2|PLATE/PLATE|0A|6|0A|#10|D2AA NVS6|
|B5|P3|PLATE/HALL|12|7|12|#11|D554 NVS6|
|B5|P4|PLATE/CHORUS|22|8|22|#15|E000 NVS7|
|B5|P5|RICH SPLIT|42|E|42|#18|E800 NVS7|

### Label tables (SBC3 12B7)
- Pointer table at 98F6: table 1 → 98FAh, table 2 → A1B2h. Entries are fixed-width 12-byte ASCII.
- Table 1 (98FA–A1B1, 186 entries):
  - bank names at indices 1–5 (`HALLS ROOMS PLATES EFFECTS SPLITS`)
  - parameter/slider labels (`LF DECAY`, `MID DECAY`, `CROSSOVER`, `DIFFUSION`, …)
  - program names from index 96h (`CONCERT HALL`), with more page labels after them (`SIZE 1` at ACh, `REV STOP DLY` at B4h, …)
  - page titles are pairs of consecutive labels (for example 53h/54h `LOW MID XOV` / `HFD DEP PDL`)
- Table 2 (A1B2–A445, 55 entries): units (`SEC MSEC HZ KHZ % METERS`), UI and tape prompts, and the option prompts `DYN DECAY[X]`, `MODE ENH [X]`, `DECAY OPT[X]` (0Fh–11h) with `1=ON 0=OFF` (12h).

## 5. Boot flow

1. Reset → 0000 `JMP 003Bh`.
2. 003B: set up the stack (`LXI SP,4000h`) and initialise the 8251 USART on ports EEh/EFh (004A). USART interrupts go through RST 7 → 03EE.
3. 00CC: `LDA B800h` reads the first byte of the NVS microcode record table (the NVS-present check).
   - ≠0 → `JMP 813Bh`, the NVS1 main application.
   - =0 → 0089.
4. 0089 is also where the power-on handshake goes (a LARC byte C8h at 0084). It runs the self-tests in sequence and then returns to 00CC:
   - the ROM checksum test at 0703–0768, which checks SBC 2 KiB blocks against indices 1–4 and NVS 4 KiB blocks from 8000 against indices 1–8;
   - the NVS RAM test (0864), SBC RAM (0897) and WCS (08F9);
   - the DSP tests and the 64 K DMEM test (0B75).

   Errors are shown through 0217 ('H') or 021D ('E'), which wait for a LARC key. The emulator enters at 00B4 and skips all of these.

## 6. Open questions

- Record header format: bytes 00–A9 of each microcode record.
- Orphan presets 44 and 09: no factory program and no microcode record `[INFERENCE: leftovers]`.
- SBC2 0800–0863: origin-0 fragment driving ports E4h–E7h `[INFERENCE: dead leftover test code]`.
- 2-byte gaps at xFFE/x7FE (BFFE–EFFE): contents listed in §3, purpose `[unknown]`.
- SBC1 0003 byte (11h) `[INFERENCE: checksum-adjust byte]`, and likewise NVS8 FFFE (DAh).
- Per-routine mapping of the SBC2 0991–0FFF DSP tests to the menu items `[INFERENCE]`.
- SBC3 102B–10AA word tables: purpose `[unknown]`.
