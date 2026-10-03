// Machine: memory map, I/O ports, the shared-timeline scheduler, boot, and live LARC requests.
#include "machine.h"

#include <algorithm>
#include <cstring>

namespace {

constexpr uint16_t kProgramTable = 0xA446;
constexpr uint16_t kLabelPointer = 0x98F6;
constexpr int kLabelWidth = 12;
// Warm restart (DI; LXI SP,4000h; EI), then 00CC -> JMP 813Bh: skips the self-tests and
// the LARC handshake.
constexpr uint16_t kBootEntry = 0x00B4;
constexpr uint16_t kProgramHook = 0x8163;  // CALL 13B6h with A = program id
constexpr uint16_t kRecordPtr = 0x3E01;
constexpr uint16_t kMainLoop = 0x8169;       // NVS1 main-loop top: CALL 82CF, no live registers
constexpr uint16_t kSliderHandler = 0x8614;  // LARC slider moved: B = slider, C = position
constexpr uint16_t kRebuild = 0xA791;        // program rebuild (full reprogram when 3C72 != 3CA2)
constexpr uint16_t kLoadBuild = 0x1402;      // program load: CALL A791 after the preset copy
constexpr uint16_t kSliderState = 0x3C20;    // + slider: catch state, 01 = tracking
constexpr uint16_t kPageNumber = 0x3C33, kStorageIndex = 0x3C34;
constexpr uint16_t kDecayState = 0x3C51, kLoadedProgram = 0x3C72;
constexpr uint16_t kOptions = 0x3CCD, kPageMask = 0x3DF8;
constexpr uint16_t kSliderBase = 0x3CA3;  // + 6 * storage index + slider

// ROM byte at 8080 address `a`; 0xFF outside the ROM ranges.
uint8_t romByte(const RomImage& r, uint32_t a) {
    if (a < 0x1800) return r.sbc[a];
    if (a >= 0x8000 && a < 0x10000) return r.nvs[a - 0x8000];
    return 0xFF;
}

uint8_t byteSum(const uint8_t* p, size_t n) {
    uint8_t sum = 0;
    for (size_t i = 0; i < n; i++) sum = uint8_t(sum + p[i]);
    return sum;
}

}  // namespace

bool romChecksumsOk(const RomImage& r) {
    for (int i = 0; i < 3; i++)
        if (byteSum(r.sbc + 0x800 * i, 0x800) != i + 1) return false;
    for (int i = 0; i < 8; i++)
        if (byteSum(r.nvs + 0x1000 * i, 0x1000) != i + 1) return false;
    return true;
}

void listPrograms(const RomImage& r, ProgramList& out) {
    out.count = 0;
    const uint16_t labels =
        uint16_t(romByte(r, kLabelPointer) | romByte(r, kLabelPointer + 1) << 8);
    // Entries: [id][bank][prog#][label idx][layout].
    for (uint32_t a = kProgramTable; a + 5 <= 0x10000 && romByte(r, a) != 0xFF &&
                                     out.count < int(sizeof out.items / sizeof *out.items);
         a += 5) {
        Program& p = out.items[out.count++];
        p.id = romByte(r, a);
        const uint32_t l = labels + uint32_t(kLabelWidth) * romByte(r, a + 3);
        int n = 0;
        for (int i = 0; i < kLabelWidth && l + i < 0x10000; i++) p.name[n++] = char(romByte(r, l + i));
        while (n > 0 && p.name[n - 1] == ' ') n--;
        p.name[n] = '\0';
    }
}

Machine::Machine() : cpu(*this) {}

void Machine::reset(const RomImage& roms, int16_t* dmem) {
    roms_ = roms;
    std::memset(nvsRam, 0, sizeof nvsRam);
    std::memset(sbcRam, 0, sizeof sbcRam);
    dsp.reset(dmem);
    cpu.reset();
    cpuTicks = dspTicks = stallTo = runningSince = 0;
    continuous = false;
    halted = true;
    programId = 0;
    hookFired = false;
    capturing = false;
    headroom[0] = headroom[1] = 0;
    io_ = nullptr;
    left_ = 0;
    loopLen = 0;
    recordPtr = 0;
    bootSeconds = 0;
    wcsWritesAudio = 0;
    inputUnderruns = 0;
    reqHead_ = reqCount_ = 0;
    inCall_ = false;
    callSp_ = 0;
    bootOptMask_ = bootOptBits_ = 0;
    bootOptApplied_ = false;
}

// The SBC stalls on WCS access while the DSP runs: the access completes at the
// next PROT step. The DSP runs ahead to it (at most 256 steps, two passes of the
// store: an emulator bound so a program without PROT steps cannot stall forever),
// and stallTo makes cpuStep() move the CPU clock past the stall.
void Machine::wcsStall() {
    if (!dspRunning()) return;
    for (int left = 256; left > 0;) {
        const Dsp::RunResult r = dsp.run(left, true);
        dspTicks += int64_t(kTicksPerDspStep) * r.steps;
        left -= r.steps;
        if (r.boundary) sampleBoundary();
        if (r.prot) break;
    }
    stallTo = dspTicks;
}

uint8_t Machine::read(uint16_t a) {
    if (a < 0x1800) return roms_.sbc[a];
    if (a >= 0x8000) {
        waits_ += kWaitStates;
        return roms_.nvs[a - 0x8000];
    }
    if (a >= 0x2000 && a < 0x2800) {
        waits_ += kWaitStates;
        return nvsRam[a - 0x2000];
    }
    if (a >= 0x3C00 && a < 0x4000) return sbcRam[a - 0x3C00];
    if (isWcs(a)) {
        waits_ += kWaitStates;
        wcsStall();
        return dsp.wcs[a - 0x4000];
    }
    return 0xFF;
}

void Machine::write(uint16_t a, uint8_t v) {
    if (a >= 0x2000 && a < 0x2800) {
        waits_ += kWaitStates;
        nvsRam[a - 0x2000] = v;
    } else if (a >= 0x3C00 && a < 0x4000) {
        sbcRam[a - 0x3C00] = v;
    } else if (isWcs(a)) {
        waits_ += kWaitStates;
        wcsStall();
        dsp.writeWcs(uint16_t(a - 0x4000), v);
        if (capturing) wcsWritesAudio++;
    }
}

uint8_t Machine::in(uint8_t port) {
    if (port <= 0x09) waits_ += kWaitStates;  // the DSP cards' ports are off-board
    switch (port) {
    case 0x06: return uint8_t(dsp.xout);
    case 0x07: return uint8_t(uint16_t(dsp.xout) >> 8);
    case 0xEF: return 0x05;  // 8251: TxRDY | TxEMPTY, never RxRDY
    case 0x08:
    case 0x09: {
        const uint8_t v = headroom[port - 0x08];
        headroom[port - 0x08] = 0;
        return v;
    }
    default: return 0x00;  // unmodelled: diagnostic IN 00-05, 8251 data (EE)
    }
}

void Machine::setDspControl(bool newContinuous, bool newHalted) {
    const bool was = dspRunning();
    continuous = newContinuous;
    halted = newHalted;
    if (!was && dspRunning()) runningSince = dspTicks;
}

// Single-cycle mode (OUT 00): each OUT 03 runs one pass of the microprogram,
// up to the next sample boundary [INFERENCE: the SBC2 self-tests expect one
// result per OUT 03]. The pass completes before the 8080's next instruction.
void Machine::singlePass() {
    dspTicks = std::max(dspTicks, cpuTicks);
    Dsp::RunResult r;
    do {
        r = dsp.run(128, false);
        dspTicks += int64_t(kTicksPerDspStep) * r.steps;
    } while (!r.boundary);
    sampleBoundary();
}

void Machine::out(uint8_t port, uint8_t v) {
    if (port <= 0x09) waits_ += kWaitStates;  // the DSP cards' ports are off-board
    switch (port) {
    case 0x00: setDspControl(false, halted); break;      // single-cycle mode
    case 0x01: setDspControl(true, halted); break;       // continuous-run mode
    case 0x02: setDspControl(continuous, true); break;   // halt immediately
    case 0x03:  // continue; in single-cycle mode, one pass
        setDspControl(continuous, false);
        if (!continuous) singlePass();
        break;
    case 0x05: dsp.pos = 0; break;  // clear the current-position counter
    case 0x06: dsp.xin = int16_t((uint16_t(dsp.xin) & 0xFF00) | v); break;       // X-in low byte
    case 0x07: dsp.xin = int16_t((uint16_t(dsp.xin) & 0x00FF) | v << 8); break;  // X-in high byte
    default: break;  // 04 (no function), 8251 EE/EF
    }
}

void Machine::sampleBoundary() {
    if (capturing) {
        io_->frame({fpcQuantize(dsp.dac[0]), fpcQuantize(dsp.dac[1]), fpcQuantize(dsp.dac[2]),
                    fpcQuantize(dsp.dac[3])});
        int16_t l, rr;
        if (!io_->nextInput(l, rr)) {
            l = rr = 0;
            inputUnderruns++;
        }
        dsp.inL = fpcQuantize(l);
        dsp.inR = fpcQuantize(rr);
        headroom[0] |= fpcHeadroom(l);
        headroom[1] |= fpcHeadroom(rr);
        if (left_) left_--;
    } else {
        dsp.inL = dsp.inR = 0;
    }
}

void Machine::cpuStep() {
    // Program select (8163, CALL 13B6h): substitute the requested program id, once per boot.
    if (cpu.pc == kProgramHook && !hookFired) {
        cpu.a = programId;
        hookFired = true;
    }
    // First preset build after the forced program copy: apply the boot options.
    if (cpu.pc == kLoadBuild && hookFired && !bootOptApplied_) {
        bootOptApplied_ = true;
        uint8_t& o = sbcRamAt(kOptions);
        o = uint8_t((o & ~bootOptMask_) | (bootOptBits_ & bootOptMask_));
    }
    if (cpu.pc == kMainLoop && hookFired) serviceRequests();
    // Off-board cycles add their wait states; a WCS access during this instruction sets
    // stallTo, and the instruction ends no earlier.
    stallTo = 0;
    waits_ = 0;
    const int t = cpu.step();
    cpuTicks = std::max(cpuTicks + int64_t(kTicksPerTState) * (t + waits_), stallTo);
}

void Machine::setBootOptions(uint8_t mask, uint8_t bits) {
    bootOptMask_ = mask;
    bootOptBits_ = bits;
}

uint16_t Machine::pageMask() const {
    uint16_t m = sbcRamWord(kPageMask);
    if (!(m & 0x0800)) m &= uint16_t(~0x0400);  // NVS3 A7AE-A7BA: page 5 present -> page 6 too
    return m;
}

bool Machine::hasPage(int page) const {
    return loopLen > 0 && page >= 1 && page <= 12 && !((pageMask() >> (16 - page)) & 1);
}

int Machine::storageIndex(int page) const {
    int n = 0;
    for (int q = 1; q < page; q++) n += hasPage(q) ? 1 : 0;
    return n;
}

bool Machine::enqueue(const Request& r) {
    for (int i = 0; i < reqCount_; i++) {
        Request& q = req_[(reqHead_ + i) % kMaxRequests];
        if (q.kind != r.kind || q.a != r.a) continue;
        if (r.kind == Request::kSlider && q.b == r.b) {
            q.c = r.c;
            return true;
        }
        if (r.kind == Request::kOption) {
            q.b = r.b;
            return true;
        }
    }
    if (reqCount_ == kMaxRequests) return false;
    req_[(reqHead_ + reqCount_) % kMaxRequests] = r;
    reqCount_++;
    return true;
}

bool Machine::moveSlider(int page, int slider, uint8_t position) {
    if (!(hasPage(page) && slider >= 0 && slider <= 5)) return false;
    return enqueue({Request::kSlider, uint8_t(page), uint8_t(slider), position});
}

bool Machine::setOption(uint8_t mask, bool on) {
    if (!(loopLen > 0 && (mask == kOptDynDecay || mask == kOptModeEnh || mask == kOptDecayOpt)))
        return false;
    return enqueue({Request::kOption, mask, uint8_t(on ? 1 : 0), 0});
}

void Machine::sliderValues(uint8_t (&out)[12][6]) const {
    int s = 0;  // storage index of the next present page
    for (int p = 1; p <= 12; p++) {
        if (hasPage(p))
            std::memcpy(out[p - 1], &sbcRamAt(kSliderBase) + 6 * s++, 6);
        else
            std::memset(out[p - 1], 0, 6);
    }
}

uint8_t Machine::options() const { return sbcRamAt(kOptions); }

// At the main-loop top: start the next queued request as a call that returns here.
// One request per pass, with a full pass in between (modulation, level detection).
// RAM goes through sbcRamAt() so Bus::write overrides do not see the injected state.
void Machine::serviceRequests() {
    if (inCall_) {
        // back at the loop top with the injected frame popped: the call is over
        if (cpu.sp == callSp_) inCall_ = false;
        return;
    }
    if (reqCount_ == 0) return;
    const Request r = req_[reqHead_];
    reqHead_ = (reqHead_ + 1) % kMaxRequests;
    reqCount_--;
    uint16_t target = 0;
    if (r.kind == Request::kSlider) {
        // The slider handler's inputs: page (3C33), storage index (3C34), the slider's
        // catch state set to tracking, B = slider, C = position.
        sbcRamAt(kPageNumber) = r.a;
        sbcRamAt(kStorageIndex) = uint8_t(storageIndex(r.a));
        sbcRamAt(uint16_t(kSliderState + r.b)) = 0x01;
        cpu.b = r.b;
        cpu.c = r.c;
        target = kSliderHandler;
    } else {
        // Toggle the bit, then table A758's follow-up: MODE ENH reprograms (3C72 = FF
        // makes A791 do a full load), DYN DECAY starts a decay ramp (3C51 = C0), DECAY
        // OPT has none.
        uint8_t& o = sbcRamAt(kOptions);
        o = uint8_t(r.b ? (o | r.a) : (o & ~r.a));
        if (r.a == kOptModeEnh) {
            sbcRamAt(kLoadedProgram) = 0xFF;
            target = kRebuild;
        } else if (r.a == kOptDynDecay) {
            sbcRamAt(kDecayState) = 0xC0;
        }
    }
    if (!target) return;
    // Enter target as if CALLed from the loop top: push kMainLoop as the return address.
    cpu.sp = uint16_t(cpu.sp - 2);
    sbcRamAt(cpu.sp) = uint8_t(kMainLoop & 0xFF);
    sbcRamAt(uint16_t(cpu.sp + 1)) = uint8_t(kMainLoop >> 8);
    callSp_ = uint16_t(cpu.sp + 2);
    cpu.pc = target;
    inCall_ = true;
}

// Same choice per step as "cpuTicks <= dspTicks ? CPU : DSP". The DSP catch-up
// runs in batches of exactly the steps that choice would make before the next
// CPU step, cut short where `done()` may change: at a sample boundary or after
// `stepCap()` steps.
template <class Pred, class Cap> void Machine::runUntil(Pred done, Cap stepCap) {
    while (!done()) {
        if (!dspRunning()) {
            cpuStep();
            dspTicks = std::max(dspTicks, cpuTicks);
            continue;
        }
        while (dspTicks < cpuTicks && !done()) {
            const int64_t due = (cpuTicks - dspTicks + kTicksPerDspStep - 1) / kTicksPerDspStep;
            const Dsp::RunResult r = dsp.run(int(std::min<int64_t>(due, stepCap())), false);
            dspTicks += int64_t(kTicksPerDspStep) * r.steps;
            if (r.boundary) sampleBoundary();
        }
        if (done()) break;
        cpuStep();
    }
}

bool Machine::runFrom(uint16_t pc, uint16_t stopPc, double maxSeconds) {
    capturing = false;
    io_ = nullptr;
    cpu.pc = pc;
    cpu.sp = 0x4000;
    cpu.ie = false;
    const int64_t cap = cpuTicks + int64_t(maxSeconds * double(kMasterHz));
    bool timedOut = false;
    runUntil(
        [&] {
            if (cpu.pc == stopPc) return true;
            if (cpuTicks >= cap) return timedOut = true;
            return false;
        },
        // Both conditions change only in CPU steps.
        [] { return int64_t(1) << 30; });
    return !timedOut;
}

bool Machine::boot(uint8_t id, const char** err) {
    programId = id;
    hookFired = false;
    bootOptApplied_ = false;
    capturing = false;
    io_ = nullptr;
    cpu.pc = kBootEntry;
    cpu.sp = 0;  // 00B4 sets SP itself
    cpu.ie = false;
    const int64_t cap = 30 * kMasterHz;
    bool timedOut = false;
    runUntil(
        [&] {
            if (dspRunning() && dspTicks - runningSince >= kMasterHz) return true;
            if (cpuTicks >= cap) return timedOut = true;
            return false;
        },
        // Steps until the 1 s DSP-run condition turns true (>= 1 while not done).
        [&] { return (runningSince + kMasterHz - dspTicks + kTicksPerDspStep - 1) / kTicksPerDspStep; });
    if (timedOut) {
        *err = hookFired ? "firmware did not start the DSP"
                         : "firmware never reached program select (8163)";
        return false;
    }
    loopLen = dsp.lastLoopLen;
    recordPtr = sbcRamWord(kRecordPtr);
    bootSeconds = double(cpuTicks) / double(kMasterHz);
    return true;
}

bool Machine::run(size_t boundaries, SampleIo& io) {
    io_ = &io;
    left_ = boundaries;
    capturing = true;
    int64_t stoppedSince = -1;
    bool stalled = false;
    runUntil(
        [&] {
            if (left_ == 0) return true;
            if (dspRunning()) {
                stoppedSince = -1;
            } else if (stoppedSince < 0) {
                stoppedSince = cpuTicks;
            } else if (cpuTicks - stoppedSince >= kMasterHz) {
                return stalled = true;
            }
            return false;
        },
        // `left_` changes only at boundaries; dspRunning() only in CPU steps.
        [] { return int64_t(1) << 30; });
    return !stalled;
}
