// Discrete DSP of the 224X (ARU, DMEM and T&C cards) executing the WCS
// microprogram step by step, and the FPC converter rules (fpcQuantize, fpcHeadroom).
// Field layout: docs/dsp_microcode.md §3; arithmetic: docs/aru_fpc.md.
#pragma once
#include <cstdint>

#if defined(__ARM_FEATURE_SAT)
#include <arm_acle.h>
#else
#include <algorithm>
#endif

// Clamp to the ARU's 19-bit range: the 20-bit adders carry two sign bits,
// and U42 forces the limit on the addend's side when they differ.
inline int32_t sat19(int32_t x) {
#if defined(__ARM_FEATURE_SAT)
    return __ssat(x, 19);
#else
    return std::min<int32_t>(std::max<int32_t>(x, -262144), 262143);
#endif
}

// One WCS step, decoded once per WCS write (active-low bits already inverted).
// The masks (-1 or 0) let the hot loop select without branching; they are
// int8_t because the signed byte load sign-extends them for free.
struct DspStep {
    uint16_t low;       // bits 0-15 as stored: DMEM offset or I/O sub-op bits
    uint8_t op;         // 0 MR, 1 MW, 2 I/O, 3 none
    uint8_t wa, ra;     // register write / read address
    uint8_t ioSrc;      // I/O bus source: 0 ADC, 1 X input register, 2 result, 3 none
    uint8_t dacMask;    // I/O DAC writes: bit0 A, bit1 B, bit2 C, bit3 D
    bool prot;          // SBC access slot
    bool wrX, reset;    // I/O: write the X output register / clear the program counter
    int8_t mrMask;      // MR: the bus is the DMEM cell
    int8_t mwMask;      // MW: the bus is the result register, stored to the cell
    int8_t sameMask;    // ra == wa: the multiplier sees this step's bus
    int8_t xferMask;    // XFER: load the result register
    int8_t zeroMask;    // ZERO: clear the accumulator (to -1, docs/aru_fpc.md §2)
    // The multiply-accumulate with the sign folded in: it adds
    // sHi*x + s1*(x >> 1) + s0*(x >> 2) + sC, which is ±P of docs/aru_fpc.md §2
    // with P = (m >> 2)(x + 1) + m1((x >> 1) + 1) + m0((x >> 2) + 1).
    int8_t sHi;         // ±(m >> 2)
    int8_t s1, s0;      // ±(bit 1 of m), ±(bit 0 of m)
    int8_t sC;          // ±((m >> 2) + bit 1 + bit 0)
};

class Dsp {
public:
    Dsp();

    // Power-on state; `dmem` (65536 words, caller-owned) is zeroed.
    void reset(int16_t* dmem);
    void writeWcs(uint16_t offset, uint8_t v);  // offset 0..511 into 4000-41FF

    uint8_t wcs[512];       // 8080 view of 4000-41FF, init 0xFF (FF step = no-op)
    DspStep steps[128];     // indexed by pc: 8080 step n is steps[127 - n]
    int16_t* dmem = nullptr;  // 64 Kwords
    // X registers on the DMEM card: the 8080 writes xin (OUT 06/07) and the
    // DSP reads it (RD XREG); the DSP writes xout (WR XREG) and the 8080
    // reads it (IN 06/07). SBC2 0A57-0A6A checks that they are separate.
    int16_t reg[4], result, xin, xout;
    // Accumulator in 1/8 data LSB, 19-bit saturating. The hardware holds the
    // complement (its data bus is inverted), so a cleared accumulator is -1
    // here and the result register takes acc >> 3 (docs/aru_fpc.md §2).
    int32_t acc;
    uint16_t pos;        // current-position counter
    uint8_t pc;          // WCS address 0..127
    bool delaySlot;      // the previous step asserted RESET
    int adcCount;        // RDAD reads since the sample boundary
    int16_t dac[4];      // A, B, C, D held values
    int16_t inL, inR;    // current input frame (already FPC-quantised)
    int stepsThisSample;  // steps run since the last sample boundary
    int lastLoopLen;      // steps in the last completed sample

    struct RunResult {
        int steps;      // steps executed, 1..n
        bool prot;      // the last step had PROT set
        bool boundary;  // the last step ended a sample
    };
    // Executes up to `n` (>= 1) steps with the state held in locals. Stops early
    // after a step that ends a sample, or, with `untilProt`, after a PROT step.
    inline RunResult run(int n, bool untilProt);

private:
    // Loop shape, kept up to date by decode(). The program is regular when its
    // first RESET is at pc 0..126, or it has none: every sample then runs pc 0
    // up to lastPc_ (the delay slot, or 127), and the delay-slot state follows
    // from the pc. A RESET at pc 127 makes the next sample's pc 0 the delay
    // slot, which only the general loop handles.
    uint32_t resetBits_[4];  // bit pc: steps[pc].reset
    uint8_t lastPc_;
    bool hasReset_, regular_;

    void clearState();
    void decode(int n);  // 8080 step n from wcs into steps[127 - n]
    void updateLoop();
    int16_t ioStep(const DspStep& s, int16_t res);  // the rare I/O steps
    template <bool kRegular, bool kUntilProt> inline RunResult runLoop(int n);
};

inline Dsp::RunResult Dsp::run(int n, bool untilProt) {
    // The regular loop needs the delay-slot state its pc implies; a program
    // rewritten mid-sample may not match it until the next boundary.
    const bool regular =
        regular_ && pc <= lastPc_ && delaySlot == (hasReset_ && pc == lastPc_);
    if (regular) return untilProt ? runLoop<true, true>(n) : runLoop<true, false>(n);
    return untilProt ? runLoop<false, true>(n) : runLoop<false, false>(n);
}

template <bool kRegular, bool kUntilProt> inline Dsp::RunResult Dsp::runLoop(int n) {
    // Hot state lives in locals; I/O-only state (xin, xout, adcCount, inL/inR,
    // dac) stays in members, touched only on the rare I/O steps.
    const DspStep* const st = steps;
    const DspStep* const end = st + lastPc_;
    int16_t* const dm = dmem;
    int16_t rfile[4] = {reg[0], reg[1], reg[2], reg[3]};
    // Keep the register file's address in a register rather than let the
    // compiler rebuild it from sp at every access.
    int16_t* r = rfile;
    asm("" : "+r"(r));
    // 16-bit values are held sign-extended in int32_t: AND/OR/XOR of
    // sign-extended operands stay sign-extended, so no re-extension is needed.
    int32_t res = result;
    int32_t a = acc;
    uint16_t p0 = pos;
    const DspStep* s = st + pc;

    // Steps 1-5 of one microinstruction.
    auto exec = [&](const DspStep& cur) __attribute__((always_inline)) {
        // 1. Bus. Every step reads its DMEM cell and writes it back (with the
        // result register for MW, unchanged otherwise): no branch on MR/MW/none.
        // For I/O and none the offset field is arbitrary but still in range.
        int16_t& cell = dm[uint16_t(p0 - cur.low)];
        const int32_t mv = cell;
        const int32_t wv = res & cur.mwMask;
        int32_t bus = (mv & cur.mrMask) | wv;
        cell = int16_t(wv | (mv & ~cur.mwMask));
        if (__builtin_expect(cur.op == 2, 0)) bus = ioStep(cur, int16_t(res));

        // 2. Transfer accumulator to result register (end of AS0): bits 18-3,
        // which a 19-bit accumulator fills exactly.
        res = ((a >> 3) & cur.xferMask) | (res & ~cur.xferMask);
        // 3. Clear accumulator (same edge; this step's product still follows).
        a |= cur.zeroMask;
        // 4. Register write (transparent: ra == wa sees the new value). The
        // read is taken before the write so it never waits on the store.
        const int32_t old = r[cur.ra];
        r[cur.wa] = int16_t(bus);
        const int32_t x = (bus & cur.sameMask) | (old & ~cur.sameMask);
        // 5. Multiply-accumulate, exact to the ROM self-test vectors: the
        // three ARU states add ((8x + 7) >> j) + 1 for each set bit of m, j =
        // 0 for bit 5 .. 5 for bit 0; the sign negates the sum exactly.
        a = sat19(a + cur.sHi * x + cur.s1 * (x >> 1) + cur.s0 * (x >> 2) + cur.sC);
    };

    // 6. Program counter. A RESET step's successor (the delay slot) ends the
    // sample; so does wrapping past step 127.
    const DspStep* last;
    bool boundary;
    int k;
    if (kRegular) {
        // Straight run from pc to the sample's last step, or n steps.
        const DspStep* const first = s;
        const DspStep* const stop = end - s < n ? end : s + (n - 1);
        for (;;) {
            exec(*s);
            if (s == stop || (kUntilProt && s->prot)) break;
            s++;
        }
        last = s;
        k = int(s - first) + 1;
        boundary = s == end;
        s = boundary ? st : s + 1;
    } else {
        bool ds = delaySlot;
        boundary = false;
        k = 0;
        for (;;) {
            const DspStep& cur = *s;
            last = s;
            k++;
            exec(cur);
            const bool slot = ds;
            ds = !slot && cur.reset;
            const unsigned next = slot ? 0u : (unsigned(s - st) + 1) & 127;
            s = st + next;
            if (next == 0) {
                boundary = true;
                break;
            }
            if (k == n || (kUntilProt && cur.prot)) break;
        }
        delaySlot = ds;
    }

    reg[0] = rfile[0];
    reg[1] = rfile[1];
    reg[2] = rfile[2];
    reg[3] = rfile[3];
    result = int16_t(res);
    acc = a;
    pc = uint8_t(s - st);
    if (kRegular) delaySlot = hasReset_ && pc == lastPc_;
    stepsThisSample += k;
    if (boundary) {
        pos = uint16_t(p0 + 1);
        adcCount = 0;
        lastLoopLen = stepsThisSample;
        stepsThisSample = 0;
    }
    return RunResult{k, last->prot, boundary};
}

// Floating-point converter, input and output: 12 converter bits with 2 gain
// bits (4 ranges, 18 dB). The word is shifted 4..1 bits (step 16..2) by
// magnitude, from 4000h/2000h/1000h (docs/aru_fpc.md §4).
int16_t fpcQuantize(int16_t x);

// Headroom-port bits (IN 08/09) for one converted input sample. 224X manual
// Table 3.3: five AIN comparators per channel at 5.0/2.24/1.12/0.56/0.28 V
// (0, -7, -13, -19, -25 dB re the 5 V clip); only the middle three set the
// gain, the outer two feed the headroom display only. The thresholds here are
// power-of-two stand-ins: 7FFFh (full scale), 4000h, 2000h, 1000h, 0800h.
// Every threshold the sample reaches sets its bit (bit 0 = full scale ..
// bit 4 = 0800h), so the code is a thermometer; bits 5-7 are undriven and
// read 0. The bit order (LA0 = full scale) is [INFERENCE]: it matches the
// firmware's headroom display table (SBC3 106B), which lights the loudest LED
// for the lowest set bit.
uint8_t fpcHeadroom(int16_t x);
