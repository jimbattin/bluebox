// Live LARC controls: Machine::moveSlider/setOption run the firmware's own
// slider handler and option actions; setBootOptions forces option bits at
// program load. Register slots and clamps are the firmware's (values measured
// on the real ROMs). ROMs load from the compile-time LEX224_ROM_DIR.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "engine.h"
#include "machine.h"
#include "romfile.h"

#ifndef LEX224_ROM_DIR
#error "LEX224_ROM_DIR must be defined"
#endif

static uint8_t sbc[0x1800], nvs[0x8000];
static int16_t dmem[65536];
static float kernel[kKernelTableSize];
static Machine m;
static Engine<1, 1> e11;
static bool ok = true;

struct Silence final : SampleIo {
    bool nextInput(int16_t& l, int16_t& r) override {
        l = r = 0;
        return true;
    }
    void frame(const DacFrame&) override { frames++; }
    long frames = 0;
};
static Silence io;

static void check(bool c, const char* what) {
    std::printf("%-58s %s\n", what, c ? "ok" : "FAIL");
    ok &= c;
}

static void boot(uint8_t id, uint8_t mask = 0, uint8_t bits = 0) {
    m.reset({sbc, nvs}, dmem);
    m.setBootOptions(mask, bits);
    const char* err = nullptr;
    if (!m.boot(id, &err)) {
        std::printf("boot %02X failed: %s\n", id, err);
        std::exit(1);
    }
}

static size_t boundaries(double seconds) {
    return size_t(seconds * double(Machine::kMasterHz) / Machine::kTicksPerDspStep / m.loopLen);
}

static void run(double seconds) {
    if (!m.run(boundaries(seconds), io)) {
        std::printf("DSP stopped\n");
        std::exit(1);
    }
}

// Runs until every request has executed, then 20 ms more.
static void settle() {
    for (int i = 0; m.pendingRequests() > 0; i++) {
        if (i >= 200) {
            std::printf("requests still pending after 2 s: %d\n", m.pendingRequests());
            std::exit(1);
        }
        run(0.010);
    }
    run(0.020);
}

// Coefficient field (M) of WCS step `step`.
static int coef(int step) { return (~m.read(uint16_t(0x4000 + 4 * step + 3)) >> 2) & 63; }

static uint32_t word(int step) {
    uint32_t w = 0;
    for (int i = 0; i < 4; i++) w |= uint32_t(m.read(uint16_t(0x4000 + 4 * step + i))) << (8 * i);
    return w;
}

// word(step) stays the same over `seconds`, sampled every 5 ms.
static bool wordSteady(int step, double seconds) {
    const uint32_t w0 = word(step);
    for (double t = 0; t < seconds; t += 0.005) {
        run(0.005);
        if (word(step) != w0) return false;
    }
    return true;
}

static void slider(int page, int s, uint8_t pos) {
    if (!m.moveSlider(page, s, pos)) {
        std::printf("moveSlider(%d, %d, %02X) refused\n", page, s, pos);
        std::exit(1);
    }
    settle();
}

static bool ram(uint16_t a, uint8_t v) {
    const uint8_t got = m.read(a);
    if (got != v) std::printf("  %04X = %02X, want %02X\n", a, got, v);
    return got == v;
}

// RMS of the last 1.0 s after a 0.3 s noise burst with MID DECAY at `mid`.
static double tailRms(uint8_t mid) {
    EngineConfig c{};
    c.roms = {sbc, nvs};
    c.dmem = dmem;
    c.kernel = kernel;
    c.sampleRate = 48000;
    c.programId = 0x01;
    c.numInputs = 1;
    c.numOutputs = 1;
    c.outMix[0] = 1;
    const char* err = nullptr;
    if (!e11.start(c, &err) || !e11.moveSlider(1, 1, mid)) {
        std::printf("engine start/moveSlider failed\n");
        std::exit(1);
    }
    const size_t pre = 4800, burst = 14400, tail = 96000, total = pre + burst + tail;
    std::mt19937 rng(20240521u);
    std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
    float in[48], out[48];
    const float* ip[1] = {in};
    float* op[1] = {out};
    double sum = 0;
    for (size_t d = 0; d < total; d += 48) {
        for (size_t i = 0; i < 48; i++) in[i] = d + i >= pre && d + i < pre + burst ? dist(rng) : 0.0f;
        if (!e11.process(ip, op, 48)) {
            std::printf("process failed\n");
            std::exit(1);
        }
        for (size_t i = 0; i < 48; i++)
            if (d + i >= total - 48000) sum += double(out[i]) * out[i];
    }
    return std::sqrt(sum / 48000.0);
}

int main() {
    std::string err;
    if (!loadRomFiles(LEX224_ROM_DIR, sbc, nvs, err)) {
        std::printf("%s\n", err.c_str());
        return 1;
    }
    initKernelTable(kernel);

    // CONCERT HALL: MID DECAY rebuilds the coefficients; clamps are the LARC's.
    boot(0x01);
    const int before = coef(44);
    slider(1, 1, 0xC0);
    std::printf("  step 44 M: %d -> %d\n", before, coef(44));
    check(ram(0x3CA4, 0xC0) && ram(0x3C53, 0xC0) && coef(44) == 24, "CONCERT HALL MID DECAY C0");
    slider(1, 0, 0xFF);
    check(ram(0x3CA3, 0xF9), "CONCERT HALL LF DECAY FF clamps to F9");
    slider(1, 2, 0x00);
    check(ram(0x3CA5, 0x08), "CONCERT HALL CROSSOVER 00 clamps to 08");
    slider(3, 5, 0xFF);
    check(ram(0x3CAE, 0xC0), "CONCERT HALL page 3 DEFINITION FF clamps to C0");

    // A queued move of the same slider is replaced.
    boot(0x01);
    const bool q1 = m.moveSlider(1, 1, 0x40), q2 = m.moveSlider(1, 1, 0xA0);
    const int pending = m.pendingRequests();
    settle();
    check(q1 && q2 && pending == 1 && ram(0x3CA4, 0xA0), "coalescing: one request, last value");

    // HALL/HALL: page 2 is the R copy at storage index 1, page 3 at index 2.
    boot(0x06);
    slider(2, 1, 0xC0);
    check(ram(0x3CAA, 0xC0) && ram(0x3CA4, 0x5F), "HALL/HALL page 2 MID DECAY -> 3CAA only");
    slider(3, 4, 0xFF);
    check(ram(0x3CB3, 0xFF), "HALL/HALL page 3 slider 4 -> 3CB3");
    uint8_t v[12][6];
    m.sliderValues(v);
    check(v[1][1] == 0xC0 && v[0][1] == 0x5F && v[2][4] == 0xFF, "HALL/HALL sliderValues readback");

    // CHORUS&ECHO: no page 1, page 3 at storage index 0.
    boot(0x0C);
    check(!m.hasPage(1) && !m.moveSlider(1, 0, 0x80) && m.hasPage(3), "CHORUS&ECHO page presence");
    slider(3, 2, 0xFF);
    check(ram(0x3CA5, 0xFF), "CHORUS&ECHO page 3 slider 2 -> 3CA5");
    m.sliderValues(v);
    check(v[0][0] == 0 && v[2][2] == 0xFF, "CHORUS&ECHO sliderValues: page 1 absent, page 3 read");

    // Options: MODE ENH off stops the modulated tap, on restarts it.
    boot(0x01);
    check(m.setOption(Machine::kOptModeEnh, false), "setOption MODE ENH off accepted");
    settle();
    check(ram(0x3CCD, 0x80) && wordSteady(108, 0.5), "MODE ENH off: 3CCD 80, step 108 steady");
    check(m.options() == 0x80, "options() reads 3CCD after MODE ENH off");
    check(m.setOption(Machine::kOptModeEnh, true), "setOption MODE ENH on accepted");
    settle();
    check(ram(0x3CCD, 0xC0) && !wordSteady(108, 0.5), "MODE ENH on: 3CCD C0, step 108 modulated");
    check(m.setOption(Machine::kOptDecayOpt, false), "setOption DECAY OPT off accepted");
    settle();
    check(ram(0x3CCD, 0x40), "DECAY OPT off: 3CCD 40");
    check(!m.setOption(0x02, true), "setOption bad mask refused");

    // Boot options take effect in the initial build.
    boot(0x01, Machine::kOptModeEnh, 0x00);
    check(ram(0x3CCD, 0x80) && wordSteady(108, 0.5), "boot with MODE ENH off: 3CCD 80, step 108 steady");

    // Audible: MID DECAY C0 tail at least 10 dB above MID DECAY 20.
    const double lo = tailRms(0x20), hi = tailRms(0xC0);
    const double db = 20.0 * std::log10(hi / lo);
    std::printf("  tail RMS: MID 20 %.3g, MID C0 %.3g (%+.1f dB)\n", lo, hi, db);
    check(db >= 10.0, "MID DECAY C0 tail >= MID DECAY 20 tail + 10 dB");

    std::printf("%s\n", ok ? "ALL OK" : "FAILURES");
    return ok ? 0 : 1;
}
