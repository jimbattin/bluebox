# ARU and FPC arithmetic

This document traces the fixed-point arithmetic of the 224X arithmetic unit (ARU) and floating-point converter (FPC) bit by bit, checks it against every DSP self-test vector in the Lexicon ROM sets, and states how `emu/` models it.

## 1. Sources

- **224X service manual** (https://therackofages.com/Lexicon/224x%20digital%20reverborator/Lexicon-224X-Service-Manual.pdf, a scan with no text layer; 1-based PDF pages):

|Content|PDF page|
|---|---|
|§3.7 ARU text (manual pp.3-12–3-14)|21–23|
|Fig. 3.4 ARU module timing|24|
|§3.8 FPC: input and output conversion|25|
|Fig. 3.5 A/D cycle timing, output gain, headroom|26|
|Fig. 3.6 D/A timing; §3.9 AIN|27|
|Table 3.3 (gain-ranger comparators)|28|
|AIN converter; §3.10 AOUT gain ranger|29|
|ARU schematic 060-01318 (one sheet)|90|
|FPC schematic 060-01320, sheets 1 and 2|92, 93|

  The ARU (drawing title "ARU – Model 224", rev 4 "updated 224X IC info") and the FPC ("Floating Point Converter – Model 224") are the same designs on the 224 and the 224X; the 224X parts list swaps in 74F parts.
- **v8.2 ROM self-tests** (`roms/224XL_v8_21`, SBC2 addresses):
  - 0942: multiplier test. Program 0A0D, copied by 0933 into 41FF down to 41F0; harness 099A; inputs 09D5 and 09DD; 20 expected words from 09E5.
  - 0C48: register-file test. Program 0CDB (41FF down to 41EC); tables 0CC3 and 0CCF, run by 0C7B (write) and 0C91 (read back).
  - 0A1D: bus and single-cycle tests. Programs 0B65 and 0B71.
- **Other ROM sets** (evidence only, not in the repository). Addresses are in the image with the ROMs concatenated from 0000:

|Set|Multiplier test|Program|Inputs|Expected|Vectors|
|---|---|---|---|---|---|
|224XL v8.1A|0617|06E2|06AA|06BA|20|
|224X v8.1|033A|0405|03CD|03DD|20|
|224 v4.4|1382|142F|1407|140F|16|
|224 v4.3|12D0|137D|1355|135D|16|
|224 v3.2|10D1|117E|1156|115E|16|
|224 TEST|12DE|13A9|1371|1381|20|
|224 TEST, second test|16B9|1766|174E|1752|10|

  Every multiplier test above uses the same 20 (or the first 16) vectors as v8.2 0942. The 224 TEST ROM's second test (harness 1714, two little-endian words per call) adds 10 vectors at m = 8, 16, 32 and 40. The 224 sets write a different microinstruction layout, but 41FA bit 7 (sign) and 41FB (ZERO, XFER, m) are the same bits as on the XL.

## 2. ARU

### Number domain
The data bus (DAB) carries the complement of the value the 8080 reads and writes through the X registers `[INFERENCE]`. Evidence:
- The self-test vectors fit the traced datapath only in the complemented domain (§3); in the 8080's own domain, no rounding rule fits them.
- The Multibus data lines are active-low, and the X registers sit on them (see [dsp_microcode.md](dsp_microcode.md) §2).
- The FPC drives the DAC80 with DA11 inverted (FPC U15, §4). The DAC80's complementary offset binary then gives an output of the right polarity only if the bus word is complemented.

Everything below this subsection is in hardware terms ("h") unless it says 8080 terms.

### Register file
- Four 16-bit words, LS670 U29–U32, with independent write (WA) and read (RA) addresses (p.22). `DAB WSTB/` writes every cycle, so address 3 serves as a pass-through.
- A step that writes register *r* from the bus and multiplies register *r* uses this step's bus value (the transparent read). SBC2 0942 step 126 and the last read of 0C48 both depend on it.

### Multiplicand: the dual-rank shift register
The 20-bit multiplicand SR19–SR0 is held in six LS194s:
- even bits in U15, U18, U4 and odd bits in U16, U17, U3 (p.22);
- at load (AS0), SR19 = SR18 = F15 (U15 A and U16 A), SR17–SR3 = F14–F0, and SR2–SR0 = 0 (U4 B, U3 A and B grounded). That is 8·h with the sign bit duplicated;
- the shift-right serial input of U15 and U16 is their own QA (sign fill); U15 QD feeds U18, and U18 QD feeds U4. So each shift in AS1 and AS2 is an arithmetic shift right by 2.

### Partial products
- Ten LS00 NANDs (U14, U26–U28, U40, U41, U50–U53) gate the direct output SR*n* into adder input B*n* with M1, and SR*n*+1 into A*n* with M0 (bit 19 of the shifted input is SR19). M1 and M0 are `M1/` and `M0/` from the T&C, through U54 (LS14).
- First-stage adders: five LS283s (U13, U24, U25, U38, U39 per the parts list; U13 is bits 3–0 and U24 bits 19–16). U13's carry-in is tied to +5 (p.22: "a negative logic adder because the nand gates provide an inversion, and the carry input of the adder is tied high"). The stage therefore forms S = ~D + ~H + 1 = ~(D + H).
- The partial-product register is U10, U11 (LS377) and U12 (LS175): 20 bits, clocked by `ARUCK`.
- The coefficient pairs are (m5, m4) in AS0, (m3, m2) in AS1 and (m1, m0) in AS2. The right shifts require this order, and the m = 40 vectors (101000b) confirm it.

### Sign
- U5–U9 (LS86) XOR the partial product with a common input derived from `CSIGN/`. U2 (S04, pin 5 → 6) inverts that input into the carry-in of U19, the low adder of the second stage.
- A positive coefficient passes D + H; a negative one adds ~(D + H) + 1 = −(D + H). The negation is exact. The 8080's bit 23 = 1 means positive, as [dsp_microcode.md](dsp_microcode.md) §3 has it.

### Accumulate and saturate
- Second stage: U19–U23 (S283) add the signed partial product to the accumulator outputs AC19–AC0.
- U42 (LS86) XORs sum bits 19 and 18 (U23 pins 10 and 13). A difference switches the LS157 muxes U33–U37 to the forced value:
  - bits 19 and 18 take the addend's bit 19 (U23 B3, pin 11);
  - bits 17–0 take its complement (U2 pins 9 → 8).

  The result is a clamp to [−2¹⁸, 2¹⁸ − 1] on the addend's side. Since the accumulator is always in range, overflow can only go in the addend's direction, and a sum that wraps the 20-bit adder still has bits 19 and 18 unequal. The clamp is therefore exact for every reachable sum. The throwaway model checked the closed form below against a 20-bit wrapping gate model: every *m*, both signs, *x* within 768 of either end of the range plus every 97th value between, and random and extreme accumulator values.
- The accumulator is five LS163s (U45–U49) with LOAD grounded (always load) and CLR = `ZERO/` (synchronous clear). "The counting function is not used" (p.23).

### Result register and timing
- U44 and U43 (LS374) latch mux outputs PP3–PP18 onto DAB0–DAB15 on `XFER CK`; `RDREG/` enables their outputs. The result is bits 18–3 of the value being loaded into the accumulator.
- p.23: the result of a multiply-accumulate is complete at the end of AS0 of the next cycle, and `XFER CK` and the `ZERO` clear both act at that instant. The vectors settle how this maps to steps:
  - an `XFER` in step *n* captures everything through step *n* − 1;
  - a `ZERO` in step *n* clears before step *n*'s own partial products are added;
  - an `XFER` in step *n* is visible to `RD RREG` in step *n* + 1.
- An `MW` in a step that also has `XFER` stores the result register as it was before that `XFER` `[INFERENCE]`. Every record's allpass chain relies on this: CONCERT HALL pc 5 and 9 store values that the `XFER`s at pc 3 and 7 produced (`tools/ucode_dump.py`). The manual's note on Fig. 3.3, "critical timing path for DIN is XFER CK to result register", suggests the opposite; that remains an open question (§6).
- The `SAT` output (connector pin A40) leaves the top edge of the drawing. `IN 03` bit 0 is `SAT/` (memory_map.md §6). Its source gate and whether it latches are `[unknown]`.

### The model in 8080 terms
Let A = ~acc (the accumulator complemented back into 8080 terms), in units of 1/8 data LSB. Because ~ commutes with arithmetic shifts and maps the clamp range onto itself:
- `ZERO`: A = −1 (hardware zero);
- multiply-accumulate of register value *x* by ±*m*/32: A = clamp₁₉(A ± P), where

  P = Σ over the set bits *b* of *m* of (((8*x* + 7) >> (5 − *b*)) + 1) = (*m* >> 2)(*x* + 1) + *m*₁((*x* >> 1) + 1) + *m*₀((*x* >> 2) + 1);
- `XFER`: result = A >> 3.

The "+ 7" is the hardware's three zero LSBs seen through the complement; the "+ 1" per bit is the NAND stage's ~(·). Consequences, all confirmed by the vectors:
- ×(+1) is exact: A = −1 + 8(*x* + 1) = 8*x* + 7, and 8*x* + 7 >> 3 = *x*;
- ×(−1) gives −*x* − 2 (5555 → AAA9);
- the negative result is the bitwise NOT of the positive one whenever P is not a multiple of 8 (16 of the 20 0942 vectors);
- saturation gives 7FFF and 8000 with no separate 16-bit clamp: bits 18–3 of a 19-bit value already fit.

The previous model, floor((*x*·*m*) >> 5) negated, a 20-bit clamp in data LSBs and a 16-bit clamp on `XFER`, matched only the 4 saturating 0942 vectors.

## 3. ROM self-test vectors

Each row is one pass: `ZERO` and a multiply of *x* in one step, `XFER` in the next, result read back through X. "✗" marks a mismatch.

|Input|Coefficient|Expected|Previous model|New model|Source|
|---|---|---|---|---|---|
|5555|−8/32|EAAA|EAAB ✗|EAAA|224 TEST 16B9|
|AAAA|−8/32|1555|1556 ✗|1555|224 TEST 16B9|
|5555|+16/32|2AAA|2AAA|2AAA|224 TEST 16B9|
|AAAA|+16/32|D555|D555|D555|224 TEST 16B9|
|5555|+21/32|3800|37FF ✗|3800|multiplier test, all sets|
|6666|+21/32|4333|4332 ✗|4333|multiplier test, all sets|
|9999|+21/32|BCCD|BCCC ✗|BCCD|multiplier test, all sets|
|AAAA|+21/32|C800|C7FF ✗|C800|multiplier test, all sets|
|5555|−21/32|C7FF|C801 ✗|C7FF|multiplier test, all sets|
|6666|−21/32|BCCC|BCCE ✗|BCCC|multiplier test, all sets|
|9999|−21/32|4332|4334 ✗|4332|multiplier test, all sets|
|AAAA|−21/32|37FF|3801 ✗|37FF|multiplier test, all sets|
|5555|+32/32|5555|5555|5555|224 TEST 16B9|
|AAAA|+32/32|AAAA|AAAA|AAAA|224 TEST 16B9|
|5555|−32/32|AAA9|AAAB ✗|AAA9|224 TEST 16B9|
|AAAA|−32/32|5554|5556 ✗|5554|224 TEST 16B9|
|5555|−40/32|9554|9556 ✗|9554|224 TEST 16B9|
|AAAA|−40/32|6AAA|6AAC ✗|6AAA|224 TEST 16B9|
|5555|+42/32|7000|6FFF ✗|7000|multiplier test, all sets|
|6666|+42/32|7FFF|7FFF|7FFF|multiplier test, all sets|
|9999|+42/32|8000|8000|8000|multiplier test, all sets|
|AAAA|+42/32|9000|8FFF ✗|9000|multiplier test, all sets|
|5555|−42/32|8FFF|9001 ✗|8FFF|multiplier test, all sets|
|6666|−42/32|8000|8000|8000|multiplier test, all sets|
|9999|−42/32|7FFF|7FFF|7FFF|multiplier test, all sets|
|AAAA|−42/32|6FFF|7001 ✗|6FFF|multiplier test, all sets|
|3333|+63/32|64CE|64CC ✗|64CE|multiplier test (m = 63 group absent in 224 v3.2–v4.4)|
|3FFF|+63/32|7DFF|7DFE ✗|7DFF|multiplier test (idem)|
|C000|+63/32|8202|8200 ✗|8202|multiplier test (idem)|
|CCCC|+63/32|9B33|9B31 ✗|9B33|multiplier test (idem)|

The v8.2 multiplier test (0942) runs five groups: 41FA/41FB = 7E/A9 (−21), FE/A9 (+21), 7E/55 (−42), FE/55 (+42) on 5555, AAAA, 6666, 9999, then FE/01 (+63) on 3333, CCCC, 3FFF, C000. The harness writes X high byte first, and each `OUT 03` runs one pass.

**Register-file test (0C48).** Step 126 reads X into register WA and multiplies register RA by +1. 0C7B writes 5555/6666/7777/EEEE (then AAAA/9999/8888/1111) into registers 0–3; 0C91 reads registers 0–3 back. The last read has WA = RA = 3 and expects EEEE (1111). That is the value 0C7B wrote to X, not the result the DSP wrote to X in the previous pass. So **X is two registers**: the 8080 writes one (read by `RD XREG`) and reads the other (written by `WR XREG`). The bus test agrees: with the DSP halted, `OUT 07` AAh followed by `IN 07` must not return AAh (SBC2 0A57–0A60, error if equal).

`emu/tests/aru_test.cpp` replays 0942 and 0C48 on `Dsp` alone from the ROM image (28 checks). Before this change it failed the 16 marked 0942 vectors; now it passes all of them.

`emu/tests/selftest_test.cpp` runs the ROM's whole power-on self-test sequence (0703 to 00CC) as firmware on the emulated machine, with each `OUT 03` in single-cycle mode running one pass. ROM checksums, NVS, SBC and WCS RAM, 0C48, 0942 and the 64 K DMEM test pass. Of the 20 checks in the bus test 0A1D, the 15 that fail need the diagnostic ports IN 00–05, or the halted DSP decoding its parked step `[INFERENCE]`: with step 127 set to RD XREG + WR XREG and the DSP halted, `OUT 06/07` must appear at `IN 06/07` (0AB8, 0ADB, 0AE3). Neither is modelled. As negative controls, adding 1 to every MAC fails 0C48, 0942 and the DMEM test, and cutting DMEM to 32 K fails the DMEM test with code 02, as [memory_map.md](memory_map.md) §5 predicts.

## 4. FPC

### Input (floating to fixed)
- The AIN's five comparators sit at 5.0, 2.24, 1.12, 0.56 and 0.28 V, where 5 V is the onset of ADC clipping (Table 3.3, p.28). Only the middle three choose the gain: 0, 6, 12 or 18 dB, coded as IGA1, IGA0 = 00, 01, 10, 11. The 5.0 V and 0.28 V comparators feed only the headroom display. "A gain of 6 dB is added whenever a signal falls below 45% of the clipping level."
- The 12-bit SAR result AD11–AD0 loads into U28 (AD11–AD8), U38 (AD7–AD4) and U39 (AD3–AD0). All four inputs of U27, the top nibble, take AD11: a sign extension to 16 bits. U39's shift-left serial input is grounded (zero fill).
- Counter U16 loads 1, IGA1, IGA0 (bit 2 preset) and counts up. The register shifts left until QC QB QA = 000: 4, 3, 2 or 1 shifts for IGA 00, 01, 10, 11 (p.25). The input word is therefore the 12-bit code times 16, 8, 4 or 2. U25 and U26 (LS244) drive it onto the DAB on `RD AD/`.
- In 8080 terms the shifted-in bits are ones, because the bus is complemented `[INFERENCE]`: word = code·2ᵏ + 2ᵏ − 1. Where the steps fall against the analog input depends on the SAR's offset trim ("offset to deliver a zero code for 0-V input", p.29), which is `[unknown]`.

### Output (fixed to floating)
- The 16-bit value from the DSP (double buffer U36/U37, LS377) loads into shift register U23 (bits 15–12), U24, U34 and U35 (bits 3–0). The output gain counter U43 counts up from 0.
- The register shifts left until U13 (LS86, bits 15 and 14) sees the two MSBs disagree, or U43 has counted three times; U14 (NOR) forms `STOP/`. U43 gives OGA1/OGA0.
- DA0–DA11 are bits 4–15 of the shifted word, so the low bits are truncated. DA11 is inverted by U15 (LS04) for the DAC80.
- The AOUT attenuates by 1, 1/2, 1/4 or 1/8 (±0.05%) per OGA (§3.10, p.29).
- The output is therefore exactly floor(*y* / 2ᵏ)·2ᵏ, with *k* = 4 if |*y*| ≥ 4000h, 3 if ≥ 2000h, 2 if ≥ 1000h, else 1, where |*y*| = *y* XOR its sign (a digital rule; the complement does not affect it).

### Headroom
- Ten LS109 JK flip-flops (U20, U21, U31, U32, U33), five per channel, take K̄ = LA0/–LA4/ and are clocked by `CH1`/`CH1/`. An LS367 per channel (U19, U30) drives them onto DATA0/–DATA4/ on HR1/ or HR2/, and an LS123 (U29) one-shot presets them after each read (sheet 2, p.93).
- The drawing buses the LA lines, so which comparator reaches which data bit cannot be read from it. The rev 2 note (8-14-79, "LA4/–LA0/ now LA0/–LA4/") says the order was reversed once. The bit order stays `[INFERENCE]` ([parameters.md](parameters.md) §4.1).

### Emulator model
- `fpcQuantize` applies the output rule above to both paths: the DAC words (exact) and the input (the power-of-two stand-in for the analog thresholds).
- `fpcHeadroom` uses power-of-two stand-ins for all five comparators: 7FFFh, 4000h, 2000h, 1000h, 0800h.
- The previous `fpcQuantize` had five ranges, with steps 16…1 and a step of 1 below 0800h, which the hardware cannot produce.

### Analog-threshold variant (not implemented)
The comparators as fractions of the 5 V clip are 1.0, 0.448, 0.224, 0.112 and 0.056. As 15-bit magnitudes (×32768, truncated) they are 7FFF, 3958h, 1CACh, 0E56h and 072Bh. An input path following the analog thresholds would change gain at 3958h, 1CACh and 0E56h instead of 4000h, 2000h and 1000h. That needs a separate `fpcInput`; the DAC path stays on the power-of-two rule, which is digital. `fpcHeadroom` would then use all five values.

## 5. Emulator mapping

|Hardware|Code|
|---|---|
|XFER, ZERO, MAC|`Dsp::runLoop` (`emu/src/dsp.h`) steps 2, 3, 5: `res = a >> 3`, `a \|= zeroMask`, `a = sat19(a + sHi·x + s1·(x >> 1) + s0·(x >> 2) + sC)`, which is ±P|
|Coefficient pairs, sign|`Dsp::decode` (`dsp.cpp`): the sign folded into `sHi` = ±(*m* >> 2), `s1`, `s0` = ±bits 1 and 0, `sC` = ±(their sum); `zeroMask`|
|19-bit clamp (U42, U33–U37)|`sat19` (`__ssat(x, 19)` on ARM)|
|Accumulator|`Dsp::acc`, 8080 terms, 1/8 LSB (A above)|
|X registers|`Dsp::xin` (OUT 06/07, `RD XREG`), `Dsp::xout` (`WR XREG`, IN 06/07); `Machine::in`/`out`|
|FPC|`fpcQuantize`, `fpcHeadroom` (`dsp.cpp`), called by `Machine` on `dsp.inL/inR` and `io_->frame`|

Remaining differences:
- The three ARU states are one clamped add. This is exact, because the terms of one multiply all have the same sign (§2).
- `clearState()` sets `acc` to 0, not to the cleared value −1. The power-on content is arbitrary on hardware, and every program clears before use.
- The input quantisation uses floor, not the ones fill, and power-of-two thresholds (§4).
- `IN 03` (`SAT/` and the ARU state bits) reads 0.

**Audio impact.** All 22 programs were rendered with `--duration 10 --tail 5` on `guitar.wav`, before and after this change (ARU, FPC and X together). ch0 = DAC A, ch1 = DAC C. ΔRMS is from `tools/wav_compare.py`, and all renders pass it. DC is the mean of the last 3 s in 16-bit LSBs. The loop lengths are unchanged. No program's level moved by 0.2 dB, so `kWetTrim` is unchanged.

|#|Program|ΔRMS ch0 / ch1 (dB)|DC before, ch0 / ch1 (LSB)|DC after, ch0 / ch1 (LSB)|
|---|---|---|---|---|
|1|CONCERT HALL|+0.000 / −0.000|−14.4 / +5.2|+11.6 / −6.7|
|2|BRIGHT HALL|+0.000 / +0.000|−15.4 / −15.5|+13.5 / +13.6|
|3|DARK HALL|+0.000 / +0.000|−20.4 / −23.3|+17.5 / +20.4|
|4|ROOM|+0.000 / +0.000|−10.4 / +3.7|+8.7 / −5.8|
|5|SMALL ROOM|+0.000 / +0.000|−18.2 / −17.5|+17.4 / +17.7|
|6|CHAMBER|+0.000 / +0.000|−6.8 / −6.8|+8.1 / +8.1|
|7|RICH CHAMBER|−0.000 / −0.000|−3.3 / −4.3|+3.9 / +2.9|
|8|PLATE|+0.000 / +0.000|+3.8 / −13.5|−4.0 / +11.9|
|9|SMALL PLATE|+0.000 / +0.000|+5.4 / −14.2|−5.8 / +13.4|
|10|CD PLATE A|−0.002 / −0.002|+5.5 / −5.5|−5.0 / +4.0|
|11|CD PLATE B|+0.000 / +0.000|−15.1 / −12.2|+14.3 / +9.9|
|12|CHORUS&ECHO|+0.000 / +0.000|−10.1 / −14.5|+10.0 / +14.0|
|13|RES CHORDS|+0.002 / −0.002|−175.2 / −189.1|+197.1 / +196.4|
|14|M BAND DELAY|+0.000 / −0.003|−2.0 / −72.0|+2.0 / +4.0|
|15|HALL/HALL|+0.000 / +0.000|−21.5 / −18.1|+21.4 / +17.7|
|16|PLATE/PLATE|+0.000 / +0.000|−11.7 / −10.5|+11.3 / +10.1|
|17|PLATE/HALL|+0.000 / +0.000|−11.4 / +8.6|+10.1 / −10.5|
|18|PLATE/CHORUS|−0.000 / −0.000|−4.1 / −3.1|+1.9 / +1.9|
|19|RICH PLATE|−0.000 / +0.000|−4.6 / −3.6|+3.7 / −1.4|
|20|DARK CHAMBER|+0.000 / +0.000|−3.5 / −4.4|+3.9 / +2.9|
|21|INVERSE ROOM|+0.000 / +0.000|−20.8 / −21.9|+23.0 / +23.0|
|22|RICH SPLIT|−0.000 / −0.000|−6.5 / −5.3|+5.1 / +3.6|

The truncation bias flipped sign: floor after negation biased every product downward, while the new rule adds up to one LSB per multiply. The DC magnitudes are similar. RES CHORDS' ~190 LSB is −45 dBFS. The real unit's outputs are transformer-coupled; the emulator has no DC blocker.

## 6. Open questions

- The `SAT` output's source gate, and whether `SAT/` latches: SBC2 0991 requires bit 0 = 0 after the multiplier test.
- MW together with XFER: the allpass structure says the old result is stored, but the Fig. 3.3 note names "XFER CK to result register" as the critical path for DIN. A cycle-level trace of T&C sheet 1 (U19, U48D) would settle it.
- Where the bus inversion sits: the X registers on the DMEM card, and the FPC's LS244/LS377 ends.
- The SAR's rounding and offset trim, and so how the input's ones fill and the analog thresholds combine (§4). An input model following the 3958h/1CACh/0E56h thresholds is described above but not implemented.
- The headroom bit order (§4, [parameters.md](parameters.md) §4.1).
