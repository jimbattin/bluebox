// Engine mechanics: block-size independence, Engine<1,1> vs Engine<2,4>,
// reboot isolation, sample-rate range and mono-mix averaging, on a
// deterministic synthetic stimulus. ROMs load from the compile-time
// LEX224_ROM_DIR.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "engine.h"
#include "romfile.h"

#ifndef LEX224_ROM_DIR
#error "LEX224_ROM_DIR must be defined"
#endif

static uint8_t sbc[0x1800], nvs[0x8000];
// Separate DMEM per engine: start() zeroes it, but e11 and e24 run interleaved in
// the mono-mix check and would otherwise share delay memory.
static int16_t dmem11[65536], dmem24[65536];
static float kernel[kKernelTableSize];
static Engine<1, 1> e11;
static Engine<2, 4> e24;

// Deterministic noise bursts (0.1 s on, 0.1 s off) followed by whatever
// silence the caller appends via a longer `total` in runEngine.
static std::vector<float> synthSignal(size_t n) {
    std::vector<float> x(n, 0.0f);
    std::mt19937 rng(20240521u);
    std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
    const size_t burst = 4800, gap = 4800;  // 0.1 s at 48 kHz; used as a sample count at any rate
    size_t i = 0;
    bool on = true;
    while (i < n) {
        size_t len = std::min(on ? burst : gap, n - i);
        if (on)
            for (size_t k = 0; k < len; k++) x[i + k] = dist(rng);
        i += len;
        on = !on;
    }
    return x;
}

template <class E>
static std::vector<float> runEngine(E& e, const std::vector<float>& x, size_t block, size_t total,
                                    uint32_t* und) {
    std::vector<float> y;
    y.reserve(total);
    float in[64], out[4][64];
    const float* ip[1] = {in};
    float* op[4] = {out[0], out[1], out[2], out[3]};
    for (size_t d = 0; d < total;) {
        size_t n = std::min(block, total - d);
        for (size_t i = 0; i < n; i++) in[i] = d + i < x.size() ? x[d + i] : 0.0f;
        if (!e.process(ip, op, n)) {
            std::printf("process failed\n");
            std::exit(1);
        }
        y.insert(y.end(), out[0], out[0] + n);
        d += n;
    }
    if (und) *und = e.underruns();
    return y;
}

static EngineConfig cfgFor(uint8_t id) {
    EngineConfig c{};
    c.roms = {sbc, nvs};
    c.dmem = dmem11;
    c.kernel = kernel;
    c.sampleRate = 48000;
    c.programId = id;
    c.numInputs = 1;
    c.numOutputs = 1;
    c.outMix[0] = 1;
    return c;
}

template <class E> static void start(E& e, const EngineConfig& c) {
    const char* err = nullptr;
    if (!e.start(c, &err)) {
        std::printf("start failed: %s\n", err);
        std::exit(1);
    }
}

template <class E> static void start(E& e, uint8_t id) { start(e, cfgFor(id)); }

int main() {
    std::string err;
    if (!loadRomFiles(LEX224_ROM_DIR, sbc, nvs, err)) {
        std::printf("%s\n", err.c_str());
        return 1;
    }
    initKernelTable(kernel);

    // 2 s of stimulus plus a 0.5 s tail, at 48 kHz.
    auto x = synthSignal(2 * 48000);
    const size_t total = x.size() + 48000 / 2;
    const uint8_t HALL = 0x01, PLATE = 0x02;
    bool ok = true;

    uint32_t u48, u1, u64, u24;
    start(e11, HALL);
    std::printf("latency %d\n", e11.latency());
    auto y48 = runEngine(e11, x, 48, total, &u48);
    start(e11, HALL);
    auto y1 = runEngine(e11, x, 1, total, &u1);
    start(e11, HALL);
    auto y64 = runEngine(e11, x, 64, total, &u64);
    std::printf("block 1 == 48: %d, block 64 == 48: %d, underruns %u %u %u\n", y1 == y48, y64 == y48,
                u48, u1, u64);
    ok &= y1 == y48 && y64 == y48 && !u48 && !u1 && !u64;

    start(e24, HALL);
    auto y24 = runEngine(e24, x, 48, total, &u24);
    std::printf("Engine<2,4> == Engine<1,1>: %d, underruns %u\n", y24 == y48, u24);
    ok &= y24 == y48 && !u24;

    // Reboot isolation: PLATE for a while, then CONCERT HALL again, must match
    // a fresh CONCERT HALL start bit for bit.
    start(e11, PLATE);
    std::vector<float> junk(2 * 48000);
    for (size_t i = 0; i < junk.size() && i < x.size(); i++) junk[i] = x[i];
    runEngine(e11, junk, 48, junk.size(), nullptr);
    start(e11, HALL);
    uint32_t ur;
    auto yr = runEngine(e11, x, 48, total, &ur);
    std::printf("reboot isolation: %d, underruns %u\n", yr == y48, ur);
    ok &= yr == y48 && !ur;

    // Rate extremes: no underruns, 1 s of stimulus each.
    for (uint32_t fs : {8000u, 44100u, 96000u, 192000u}) {
        for (uint8_t id : {uint8_t(0x01), uint8_t(0x02), uint8_t(0x11)}) {
            EngineConfig c = cfgFor(id);
            c.sampleRate = fs;
            auto xf = synthSignal(fs);
            for (size_t b : {size_t(1), size_t(64)}) {
                start(e11, c);
                uint32_t uu;
                runEngine(e11, xf, b, fs, &uu);
                std::printf("fs %6u id %02X block %2zu latency %4d underruns %u\n", fs, id, b,
                            e11.latency(), uu);
                ok &= uu == 0;
            }
        }
    }

    // Mono mix: CHORUS&ECHO (A=D, B=C) averaged into one output must equal the
    // mean of DAC A and DAC C taken as separate outputs, from a fresh boot.
    {
        EngineConfig c1 = cfgFor(0x0C);
        c1.outMix[0] = 0x05;  // A | C
        EngineConfig c2 = c1;
        c2.dmem = dmem24;
        c2.numOutputs = 2;
        c2.outMix[0] = 0x01;  // A alone
        c2.outMix[1] = 0x04;  // C alone
        start(e11, c1);
        start(e24, c2);

        auto xm = synthSignal(48000);
        float in[48], o1[4][48], o2[4][48];
        const float* ip[1] = {in};
        float* op1[4] = {o1[0], o1[1], o1[2], o1[3]};
        float* op2[4] = {o2[0], o2[1], o2[2], o2[3]};
        double maxd = 0, peak = 0;
        for (size_t d = 0; d + 48 <= xm.size(); d += 48) {
            std::memcpy(in, &xm[d], sizeof in);
            if (!e11.process(ip, op1, 48) || !e24.process(ip, op2, 48)) {
                std::printf("process failed\n");
                return 1;
            }
            for (int i = 0; i < 48; i++) {
                const double ref = 0.5 * (double(o2[0][i]) + o2[1][i]);
                maxd = std::max(maxd, std::fabs(ref - double(o1[0][i])));
                peak = std::max(peak, std::fabs(ref));
            }
        }
        std::printf("CHORUS&ECHO A+C mix vs mean(A, C): max |diff| %.3g (peak %.3f) underruns %u %u\n",
                    maxd, peak, e11.underruns(), e24.underruns());
        ok &= maxd < 1e-5;

        // Live switch: e11 to C alone must match e24's separate C output once the
        // resampler and de-emphasis have settled; bad arguments are rejected.
        ok &= !e11.setOutMix(0, 0) && !e11.setOutMix(0, 16) && !e11.setOutMix(1, 0x01) &&
              e11.setOutMix(0, 0x04);
        double maxc = 0, peakc = 0;
        for (size_t d = 0; d + 48 <= xm.size(); d += 48) {
            std::memcpy(in, &xm[d], sizeof in);
            if (!e11.process(ip, op1, 48) || !e24.process(ip, op2, 48)) {
                std::printf("process failed\n");
                return 1;
            }
            if (d < 4800) continue;
            for (int i = 0; i < 48; i++) {
                maxc = std::max(maxc, std::fabs(double(o1[0][i]) - o2[1][i]));
                peakc = std::max(peakc, std::fabs(double(o2[1][i])));
            }
        }
        std::printf("CHORUS&ECHO live setOutMix C vs C: max |diff| %.3g (peak %.3f) underruns %u %u\n",
                    maxc, peakc, e11.underruns(), e24.underruns());
        ok &= maxc < 1e-5;
    }

    std::printf(ok ? "ALL OK\n" : "FAILURES\n");
    return ok ? 0 : 1;
}
