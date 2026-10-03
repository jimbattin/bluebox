// 224XL mainframe: SBC 80/10 (8080 + SBC/NVS ROMs + RAM) driving the DSP, with the
// boot sequence and live LARC controls injected into the firmware.
// Both processors share one 30.72 MHz master-tick timeline:
// 1 8080 T-state = 15 ticks (2.048 MHz), 1 DSP step = 9 ticks.
// Allocation-free: ROMs and DMEM are caller-owned, everything else is fixed-size.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

#include "dsp.h"
#include "i8080.h"

using DacFrame = std::array<int16_t, 4>;  // DAC A, B, C, D

// 0x1800 bytes mapped at 0000 (SBC1-3) and 0x8000 bytes mapped at 8000 (NVS1-8).
struct RomImage {
    const uint8_t* sbc;
    const uint8_t* nvs;
};

// Each 2 KiB SBC block sums to 1, 2, 3 and each 4 KiB NVS block to 1..8.
bool romChecksumsOk(const RomImage& r);

struct Program {
    uint8_t id;
    char name[13];  // NUL-terminated, trailing spaces trimmed
};
struct ProgramList {
    Program items[32];
    int count;
};
// Factory programs in ROM-table order.
void listPrograms(const RomImage& r, ProgramList& out);

// Audio endpoint of Machine::run, called at each captured sample boundary.
struct SampleIo {
    virtual bool nextInput(int16_t& l, int16_t& r) = 0;  // false: nothing queued
    virtual void frame(const DacFrame& dac) = 0;         // FPC-quantised DAC frame

protected:
    ~SampleIo() = default;
};

class Machine : public Bus {
public:
    static constexpr int64_t kMasterHz = 30'720'000;
    static constexpr int kTicksPerTState = 15;
    static constexpr int kTicksPerDspStep = 9;
    // Wait states the 8080 spends on each off-board (Multibus) machine cycle: NVS ROM and RAM,
    // WCS, DSP ports 00-09. On-board SBC ROM, RAM and I/O have none (SBC 80/10 HRM §3.2.1).
    // An off-board write takes at least 1 (HRM Fig. 3-4), a read 0 only if XACK/ answers within
    // 191 ns of the command (HRM Table 7-2). The NVS card's XACK/ timing is [unknown]; 1 for
    // every off-board cycle is [INFERENCE].
    static constexpr int kWaitStates = 1;

    Machine();

    // Power-on state with `roms` mapped; `dmem` (65536 words) is caller-owned.
    void reset(const RomImage& roms, int16_t* dmem);

    // Boots the NVS application, forces factory program `programId` at the
    // program-select call (8163) and runs until the DSP has run for 1 s.
    bool boot(uint8_t programId, const char** err);

    // Runs the 8080 from `pc` (stack at 4000h, interrupts off) until it is about
    // to execute `stopPc`. False: `maxSeconds` of 8080 time passed first. Used
    // to run the ROM's power-on self-tests (emu/tests/selftest_test.cpp).
    bool runFrom(uint16_t pc, uint16_t stopPc, double maxSeconds);

    // Runs until `boundaries` sample boundaries have passed, exchanging one
    // input and one DAC frame with `io` at each. A WCS stall can overshoot by a
    // few boundaries; those frames also go to `io`. False: the DSP stopped for
    // 1 s of 8080 time.
    bool run(size_t boundaries, SampleIo& io);

    static constexpr uint8_t kOptDynDecay = 0x01, kOptModeEnh = 0x40, kOptDecayOpt = 0x80;
    // Option bits (3CCD) forced into the program as it loads: bits in `mask` take `bits`.
    // Call after reset(), before boot(); reset() clears it.
    void setBootOptions(uint8_t mask, uint8_t bits);
    // After a successful boot(): page 1..12 is present in the loaded program's record.
    bool hasPage(int page) const;
    // LARC slider `slider` (0-5) of `page` (1-12) moved to `position`. The firmware's own
    // handler (NVS1 8614) applies it at the next main-loop pass, clamping to the program's
    // range. A queued move of the same page/slider is replaced. False: no program booted,
    // page absent, bad arguments, or queue full.
    bool moveSlider(int page, int slider, uint8_t position);
    // Toggle an option bit (one of kOpt*) with the ROM's own follow-up action (table A758).
    // A queued toggle of the same bit is replaced. False: no program booted, bad mask, queue full.
    bool setOption(uint8_t mask, bool on);
    // After a successful boot(): the firmware's current slider registers (clamped values),
    // out[page-1][slider]; absent pages read 0.
    void sliderValues(uint8_t (&out)[12][6]) const;
    // Option bits (3CCD, kOpt*).
    uint8_t options() const;
    // Requests queued or still executing.
    int pendingRequests() const { return reqCount_ + (inCall_ ? 1 : 0); }

    int loopLen = 0;              // steps per sample of the booted program; 0 until boot() succeeds
    uint16_t recordPtr = 0;       // microcode record of the booted program (3E01)
    double bootSeconds = 0;       // emulated time since reset() when boot() finished
    int64_t wcsWritesAudio = 0;   // WCS writes during run(): live coefficient updates
    uint32_t inputUnderruns = 0;  // boundaries in run() with no input queued (fed silence)

    uint8_t read(uint16_t a) override;
    void write(uint16_t a, uint8_t v) override;
    uint8_t in(uint8_t port) override;
    void out(uint8_t port, uint8_t v) override;

protected:  // tools/lexprobe.cpp and emu/tests/selftest_test.cpp subclass Machine
    RomImage roms_{};
    // Wait states of the current instruction's off-board cycles; cpuStep() adds them to its
    // time. Declared early so that the accesses that count them address it with a short offset.
    int waits_ = 0;
    uint8_t nvsRam[0x800];  // 2000-27FF
    uint8_t sbcRam[0x400];  // 3C00-3FFF
    I8080 cpu;
    Dsp dsp;
    int64_t cpuTicks = 0, dspTicks = 0;  // each processor's position on the master-tick timeline
    int64_t stallTo = 0;  // end of the current instruction's WCS stall (0: none), see wcsStall()
    int64_t runningSince = 0;  // dspTicks when the DSP last started running (boot()'s 1 s test)
    // OUT 00/01: single-cycle / continuous-run mode; OUT 02/03: halt / continue.
    bool continuous = false, halted = true;

    // Program forced at the program-select call (8163), and whether that hook fired this boot.
    uint8_t programId = 0;
    bool hookFired = false;

    // Live LARC requests, injected as calls at the NVS1 main-loop top (see serviceRequests).
    struct Request {
        static constexpr uint8_t kSlider = 0, kOption = 1;  // kind
        uint8_t kind, a, b, c;  // kSlider: a page, b slider, c position; kOption: a mask, b on
    };
    static constexpr int kMaxRequests = 32;
    Request req_[kMaxRequests] = {};
    int reqHead_ = 0, reqCount_ = 0;
    bool inCall_ = false;  // an injected call has not returned to the main loop yet
    uint16_t callSp_ = 0;  // sp once it returns
    uint8_t bootOptMask_ = 0, bootOptBits_ = 0;
    bool bootOptApplied_ = false;
    // SBC RAM by 8080 address (3C00-3FFF). Direct access: Bus::read/write overrides
    // (lexprobe's traces) do not see it.
    uint8_t& sbcRamAt(uint16_t a) { return sbcRam[uint16_t(a - 0x3C00)]; }
    const uint8_t& sbcRamAt(uint16_t a) const { return sbcRam[uint16_t(a - 0x3C00)]; }
    uint16_t sbcRamWord(uint16_t a) const {  // little-endian
        return uint16_t(sbcRamAt(a) | sbcRamAt(uint16_t(a + 1)) << 8);
    }
    uint16_t pageMask() const;         // 3DF8 word with the A7AE-A7BA page-6 rule
    int storageIndex(int page) const;  // present pages below `page`
    void serviceRequests();            // at the main-loop top
    bool enqueue(const Request& r);    // replaces a queued request with the same key

    bool capturing = false;  // inside run(): boundaries exchange audio with io_
    // Headroom registers (IN 08 = channel 1 = L [INFERENCE], IN 09 = R): peak
    // hold of fpcHeadroom() over the input samples since the port was last
    // read; the read strobe presets the register (224 manual pp.44-45).
    uint8_t headroom[2] = {0, 0};
    // run()'s audio endpoint and the boundaries still to go.
    SampleIo* io_ = nullptr;
    size_t left_ = 0;

    bool dspRunning() const { return continuous && !halted; }
    static bool isWcs(uint16_t a) { return a >= 0x4000 && a < 0x4200; }
    void wcsStall();
    void setDspControl(bool newContinuous, bool newHalted);
    void singlePass();  // OUT 03 in single-cycle mode
    void sampleBoundary();  // DAC frame out, next input in
    void cpuStep();
    // `stepCap()`: DSP steps that can run before `done()` may change, other than
    // at a sample boundary. `done()` is evaluated after every DSP batch and before
    // every CPU step, so it may run several times per CPU step.
    template <class Pred, class Cap> void runUntil(Pred done, Cap stepCap);
};
