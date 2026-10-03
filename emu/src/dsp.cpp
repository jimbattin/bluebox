// Dsp setup, WCS decoding and the rare I/O steps (the step loop itself is inline
// in dsp.h), and the FPC rules.
#include "dsp.h"

#include <cstring>

Dsp::Dsp() { clearState(); }

void Dsp::clearState() {
    std::memset(wcs, 0xFF, sizeof wcs);
    // both cleared: decode() flips resetBits_ only when a step's RESET changes
    std::memset(resetBits_, 0, sizeof resetBits_);
    for (DspStep& s : steps) s.reset = false;
    for (int n = 0; n < 128; n++) decode(n);
    updateLoop();
    std::memset(reg, 0, sizeof reg);
    std::memset(dac, 0, sizeof dac);
    result = xin = xout = 0;
    acc = 0;
    pos = 0;
    pc = 0;
    delaySlot = false;
    adcCount = 0;
    inL = inR = 0;
    stepsThisSample = lastLoopLen = 0;
}

void Dsp::reset(int16_t* mem) {
    dmem = mem;
    std::memset(dmem, 0, 65536 * sizeof *dmem);
    clearState();
}

void Dsp::writeWcs(uint16_t offset, uint8_t v) {
    wcs[offset] = v;
    decode(offset >> 2);
}

void Dsp::decode(int n) {
    const uint32_t w = uint32_t(wcs[4 * n]) | uint32_t(wcs[4 * n + 1]) << 8 |
                       uint32_t(wcs[4 * n + 2]) << 16 | uint32_t(wcs[4 * n + 3]) << 24;
    DspStep& s = steps[127 - n];
    s.low = uint16_t(w);
    s.op = uint8_t(w >> 16 & 3);
    s.wa = uint8_t(w >> 18 & 3);
    s.ra = uint8_t(w >> 20 & 3);
    const bool io = s.op == 2;
    // I/O sub-operation bits are active-low (docs/dsp_microcode.md §3, "I/O sub-operations").
    s.ioSrc = uint8_t(s.low >> 12 & 3);  // bits 13-12: 0 RD AD, 1 RD XREG, 2 RD RREG, 3 none
    s.dacMask = 0;
    if (io && !(s.low & 0x80)) {  // WR DA/
        if (!(s.low & 0x800)) s.dacMask |= 1;  // SDAA
        if (!(s.low & 0x400)) s.dacMask |= 2;  // SDAB
        if (!(s.low & 0x200)) s.dacMask |= 4;  // SDAC
        if (!(s.low & 0x100)) s.dacMask |= 8;  // SDAD
    }
    s.prot = w >> 22 & 1;
    s.wrX = io && !(s.low & 0x40);             // WR XREG/
    const bool reset = io && !(s.low & 0x08);  // RESET
    // resetBits_ mirrors steps[].reset; the loop shape is recomputed only when it changes.
    if (reset != s.reset) {
        s.reset = reset;
        const int pc = 127 - n;
        resetBits_[pc >> 5] ^= 1u << (pc & 31);
        updateLoop();
    }
    s.mrMask = s.op == 0 ? -1 : 0;
    s.mwMask = s.op == 1 ? -1 : 0;
    s.sameMask = s.wa == s.ra ? -1 : 0;
    s.xferMask = (w >> 24 & 1) ? 0 : -1;
    s.zeroMask = (w >> 25 & 1) ? 0 : -1;
    const int8_t neg = (w >> 23 & 1) ? 1 : -1;  // sign bit: 1 = positive
    const int m = int(~w >> 26 & 0x3F);
    s.sHi = int8_t(neg * (m >> 2));
    s.s1 = int8_t(neg * (m >> 1 & 1));
    s.s0 = int8_t(neg * (m & 1));
    s.sC = int8_t(neg * ((m >> 2) + (m >> 1 & 1) + (m & 1)));
}

void Dsp::updateLoop() {
    int first = -1;
    for (int i = 0; i < 4 && first < 0; i++)
        if (resetBits_[i]) first = 32 * i + __builtin_ctz(resetBits_[i]);
    hasReset_ = first >= 0;
    regular_ = first != 127;
    lastPc_ = uint8_t(hasReset_ && regular_ ? first + 1 : 127);
}

int16_t Dsp::ioStep(const DspStep& s, int16_t res) {
    int16_t bus = 0;  // no source
    switch (s.ioSrc) {
    case 2: bus = res; break;
    case 1: bus = xin; break;
    case 0:
        // RD AD reads alternate L, R within a sample [INFERENCE] (docs/dsp_microcode.md);
        // adcCount restarts at each boundary.
        bus = (adcCount & 1) ? inR : inL;
        adcCount++;
        break;
    default: break;
    }
    if (s.dacMask & 1) dac[0] = bus;
    if (s.dacMask & 2) dac[1] = bus;
    if (s.dacMask & 4) dac[2] = bus;
    if (s.dacMask & 8) dac[3] = bus;
    if (s.wrX) xout = bus;
    return bus;
}

namespace {

// x for x >= 0, ~x (= -x - 1) for x < 0, so its bit length is the position of the
// first bit that differs from the sign: what the FPC's "two MSBs agree" test and
// the headroom thresholds measure. Always 0..7FFFh; -8000h maps to 7FFFh.
uint32_t fpcMagnitude(int16_t x) { return uint16_t(x ^ (x >> 15)) & 0x7FFF; }

}  // namespace

int16_t fpcQuantize(int16_t x) {
    // The output FPC shifts the word left while its two MSBs agree, at most 3
    // times (U13/U14/U43), and sends bits 15-4 of the shifted word to the DAC, so
    // `shift` = 4 - (normalising shifts) low bits are truncated: a step of 16, 8, 4
    // or 2. Multiplying by 1 << shift instead of shifting left keeps negative values
    // defined in C++17.
    const uint32_t v = fpcMagnitude(x);
    const int shift = v >= 0x4000 ? 4 : v >= 0x2000 ? 3 : v >= 0x1000 ? 2 : 1;
    return int16_t((x >> shift) * (1 << shift));
}

uint8_t fpcHeadroom(int16_t x) {
    const uint32_t v = fpcMagnitude(x);
    if (v < 0x800) return 0;
    const int bitLength = 32 - __builtin_clz(v);  // 12..15
    // Thresholds -24 dB (bit 4) up to the highest one reached (bit 16 - bitLength).
    const uint32_t ranges = 0x1Eu & ~((1u << (16 - bitLength)) - 1u);
    return uint8_t(ranges | (v == 0x7FFF ? 1u : 0u));  // bit 0: ADC limit (full scale)
}
