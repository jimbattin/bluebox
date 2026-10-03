// Terrarium control logic for the lex224 pedal, host-portable (no libDaisy):
// knob map and parked takeover, the FS2 hold/tap gesture, bank levers, lever and
// editor option arbitration, the LED2 program-number code, the dry/wet level
// law and the mono-output override table. Modelled on DaisyCloudSeed's
// footswitch_gestures.h, knob_bank.h and toggle_bank.h, without the takeover glide:
// the emulated 8080 applies slider moves at ~62 Hz, so there is nothing to smooth here.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "editor_protocol.h"

constexpr int kKnobs = 6;
constexpr int kBankSize = 6;
constexpr float kKnobMoveThreshold = 0.02f;  // takeover; no extra smoothing in this pedal
constexpr int kBankSettleBlocks = 300;       // 1 ms blocks
constexpr uint32_t kFlashOnMs = 150, kFlashOffMs = 150, kFlashPauseMs = 1500;

// Knob target: page 0 = the pedal's dry/wet mix, else a LARC page (1-12) and slider (0-5).
struct KnobTarget {
    uint8_t page, slider;
};
// [layer][knob], knob index 0..5 = Terrarium KNOB_1..KNOB_6; layer 0 primary, 1 secondary (FS2 held).
constexpr KnobTarget kKnobMap[2][kKnobs] = {
    {{0, 0}, {1, 0}, {1, 1}, {1, 2}, {1, 3}, {1, 4}},   // mix, LF DECAY, MID DECAY, CROSSOVER, TREBLE DECAY, DEPTH
    {{1, 5}, {3, 4}, {3, 5}, {3, 2}, {3, 3}, {3, 0}}};  // PREDELAY, DIFFUSION, DEFINITION, CHORUS, HF BANDWIDTH, LF STOP DCY

// Knob of `layer` that drives `page`/`slider` (page 0 = mix), or -1. Page-1 knobs
// also drive page 2 (the right reverb of split programs).
inline int knobFor(int layer, int page, int slider) {
    const int p = page == 2 ? 1 : page;
    for (int k = 0; k < kKnobs; k++)
        if (kKnobMap[layer][k].page == p && (p == 0 || kKnobMap[layer][k].slider == slider)) return k;
    return -1;
}

// Bank from levers 3 and 4: 00->0, 10->1, 01->2, 11->3.
inline int bankFromLevers(bool sw3, bool sw4) { return (sw3 ? 1 : 0) + (sw4 ? 2 : 0); }

// Programs in `bank` out of `programCount`: clamp(programCount - bank*6, 0, 6).
inline int bankSize(int bank, int programCount) {
    const int n = programCount - bank * kBankSize;
    return n < 0 ? 0 : n > kBankSize ? kBankSize : n;
}

// Program index after `index` within its bank, wrapping to the bank's first.
inline int nextInBank(int index, int programCount) {
    const int bank = index / kBankSize;
    const int size = bankSize(bank, programCount);
    if (size <= 0) return index;
    return bank * kBankSize + (index % kBankSize + 1) % size;
}

// Knob value 0..1 -> LARC slider position 00..FF.
inline uint8_t sliderPosition(float v) {
    const float c = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    return uint8_t(int(c * 255.0f + 0.5f));
}

// Send a new slider position? lastSent < 0 means nothing sent since the last park.
// One-step changes are ADC noise, except onto a rail.
inline bool sliderChanged(int lastSent, uint8_t pos) {
    if (lastSent < 0) return true;
    return pos != lastSent && (std::abs(pos - lastSent) >= 2 || pos == 0 || pos == 255);
}

// LED2 code for in-bank program number n (1..6), `ms` since the program started
// running: n flashes of kFlashOnMs/kFlashOffMs, then kFlashPauseMs dark, repeated.
inline bool flashLit(int n, uint32_t ms) {
    const uint32_t flash = kFlashOnMs + kFlashOffMs;
    const uint32_t t = ms % (uint32_t(n) * flash + kFlashPauseMs);
    return t < uint32_t(n) * flash && t % flash < kFlashOnMs;
}

// Dry/wet gains for mix 0..1, equal power: dry = cos, wet = sin of mix * 90 degrees.
// The wet output is nearly uncorrelated with the dry input (|r| <= 0.18 on guitar),
// so with the wet trimmed to the dry's loudness the blend stays at unity; a linear
// crossfade would dip about 3 dB at the middle. Exactly 1/0 at mix 0 and 0/1 at mix 1.
inline void mixGains(float mix, float& dry, float& wet) {
    if (!(mix > 0.0f)) {
        dry = 1.0f;
        wet = 0.0f;
    } else if (mix >= 1.0f) {
        dry = 0.0f;
        wet = 1.0f;
    } else {
        dry = std::cos(mix * 1.57079633f);
        wet = std::sin(mix * 1.57079633f);
    }
}

// Parked knob takeover (knob_bank.h semantics, no glide). Park() snapshots every
// knob; a parked knob goes live once it moves kKnobMoveThreshold from its snapshot
// and stays live until the next Park().
class KnobTakeover {
public:
    void Park(const float* positions) {
        for (int i = 0; i < kKnobs; i++) {
            snapshot_[i] = positions[i];
            live_[i] = false;
        }
        primed_ = true;
    }
    // Re-park knob `i` alone (another control moved its target); the others keep their state.
    void ParkOne(int i, float position) {
        snapshot_[i] = position;
        live_[i] = false;
    }
    // True while knob `i` is live; false before the first Park().
    bool Update(int i, float position) {
        if (!primed_) return false;
        if (!live_[i]) {
            const float d = position - snapshot_[i];
            if ((d < 0.0f ? -d : d) < kKnobMoveThreshold) return false;
            live_[i] = true;
        }
        return true;
    }

private:
    float snapshot_[kKnobs] = {};
    bool live_[kKnobs] = {};
    bool primed_ = false;
};

// FS2 rule of footswitch_gestures.h: held from Switch::Pressed() (8 down blocks)
// until FallingEdge(), so bounce during a hold cannot drop it; the release advances
// the program unless a secondary-layer knob went live during the hold.
class Fs2Gesture {
public:
    // Once per block with Switch::Pressed()/FallingEdge(). Returns true on the release
    // edge of a hold during which MarkEdited() was not called.
    bool Update(bool pressed, bool falling) {
        if (pressed) held_ = true;
        if (falling && held_) {
            const bool advance = !edited_;
            held_ = edited_ = false;
            return advance;
        }
        return false;
    }
    bool Held() const { return held_; }
    void MarkEdited() { edited_ = true; }

private:
    bool held_ = false;
    bool edited_ = false;
};

// Bank levers with a settle time, so sliding past intermediate lever
// combinations does not load their banks.
class BankSelector {
public:
    void Init(int bank) {
        settled_ = candidate_ = bank;
        count_ = 0;
        changed_ = false;
    }
    // Once per block; returns the settled bank. A new lever value settles after
    // kBankSettleBlocks consecutive blocks.
    int Update(int leverBank) {
        if (leverBank == settled_) {
            candidate_ = settled_;
            count_ = 0;
        } else if (leverBank != candidate_) {
            candidate_ = leverBank;
            count_ = 1;
        } else {
            count_++;
        }
        if (candidate_ != settled_ && count_ >= kBankSettleBlocks) {
            settled_ = candidate_;
            count_ = 0;
            changed_ = true;
        }
        return settled_;
    }
    // True once after the settled bank changed (a lever move, as an edge).
    bool TakeChange() {
        const bool c = changed_;
        changed_ = false;
        return c;
    }

private:
    int settled_ = 0, candidate_ = 0, count_ = 0;
    bool changed_ = false;
};

// Option bits wanted for the running program. Each program load starts from the
// preset's own bits (Loaded); after that a lever flip sets its bit to the lever's
// position and an editor Set() sets its bits, until the next load. Touches made
// while a program loads (after BeginLoad) are kept over the preset.
class OptionState {
public:
    void Init(uint8_t levers) { levers_ = levers; desired_ = touched_ = 0; }
    // Once per block with the lever bits; returns the desired bits.
    uint8_t Update(uint8_t levers) {
        Apply(uint8_t(levers ^ levers_), levers);
        levers_ = levers;
        return desired_;
    }
    void Set(uint8_t mask, bool on) { Apply(mask, on ? mask : 0); }
    // The running program stopped (a load begins): earlier touches no longer count.
    void BeginLoad() { touched_ = 0; }
    // A program booted with the preset's option bits: untouched bits take them.
    void Loaded(uint8_t preset) { desired_ = uint8_t((desired_ & touched_) | (preset & ~touched_)); }
    uint8_t Desired() const { return desired_; }

private:
    void Apply(uint8_t mask, uint8_t bits) {
        desired_ = uint8_t((desired_ & ~mask) | (bits & mask));
        touched_ |= mask;
    }
    uint8_t levers_ = 0, desired_ = 0, touched_ = 0;
};

// Per-program mono-output overrides by program ID: an EngineConfig::outMix mask from the
// editor's menu (barc::IsOutMixChoice), 0 = the program's default. Stored in one QSPI
// sector as a record: magic (4 bytes LE), the 256 entries, CRC-32 of those 260 bytes (LE).
class OutMixTable {
public:
    static constexpr uint32_t kMagic = 0x314D584F;  // "OXM1"
    static constexpr size_t kRecordSize = 4 + 256 + 4;
    uint8_t mix[256] = {};

    // False, with every entry 0, if the magic or CRC is wrong (an erased sector);
    // an entry that is not a menu choice loads as 0.
    bool Load(const uint8_t* rec) {
        std::memset(mix, 0, sizeof mix);
        if (Get32(rec) != kMagic || Get32(rec + 260) != barc::Crc32(rec, 260)) return false;
        for (int i = 0; i < 256; i++) mix[i] = barc::IsOutMixChoice(rec[4 + i]) ? rec[4 + i] : 0;
        return true;
    }
    void Save(uint8_t* rec) const {
        Put32(rec, kMagic);
        std::memcpy(rec + 4, mix, sizeof mix);
        Put32(rec + 260, barc::Crc32(rec, 260));
    }

private:
    static uint32_t Get32(const uint8_t* p) {
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    }
    static void Put32(uint8_t* p, uint32_t v) {
        for (int i = 0; i < 4; i++) p[i] = uint8_t(v >> 8 * i);
    }
};
