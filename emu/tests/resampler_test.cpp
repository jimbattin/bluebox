// Resampler spec check at CONCERT HALL's DSP rate D = 30.72 MHz / 9 / 105: a
// 48k -> D -> 48k round trip is flat within ±0.1 dB up to 14.5 kHz (±1 dB at 15 kHz),
// and one-way aliases and images are at least 75 dB down.
#include <cmath>
#include <cstdio>
#include <vector>

#include "resampler.h"

static float kernel[kKernelTableSize];
constexpr uint32_t kA = 48000u * 945u, kB = 30720000u;  // 48k / D, L = 105
const double D = 30720000.0 / 945.0;

static std::vector<float> run(const std::vector<float>& x, uint32_t num, uint32_t den) {
    static StreamResampler<1, 1024> r;
    r.init(kernel, num, den);
    std::vector<float> y;
    for (float v : x) {
        r.push(&v);
        float o;
        while (r.ready()) { r.pull(&o); y.push_back(o); }
    }
    return y;
}

static std::vector<float> sine(double f, double fs, size_t n) {
    std::vector<float> x(n);
    for (size_t i = 0; i < n; i++) x[i] = float(0.5 * std::sin(2 * M_PI * f * i / fs));
    return x;
}

static double rmsMid(const std::vector<float>& x) {
    size_t a = x.size() / 4, b = 3 * x.size() / 4;
    double s = 0;
    for (size_t i = a; i < b; i++) s += double(x[i]) * x[i];
    return std::sqrt(s / double(b - a));
}

static double goertzel(const std::vector<float>& x, double f, double fs) {
    size_t a = x.size() / 4, b = 3 * x.size() / 4, n = b - a;
    double re = 0, im = 0;
    for (size_t i = 0; i < n; i++) {
        double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / (n - 1));
        double ph = 2 * M_PI * f * i / fs;
        re += w * x[a + i] * std::cos(ph);
        im += w * x[a + i] * std::sin(ph);
    }
    return std::hypot(re, im);
}

int main() {
    initKernelTable(kernel);
    bool ok = true;
    const size_t n = 4 * 48000;
    for (double f : {1000.0, 5000.0, 10000.0, 14500.0, 15000.0}) {
        auto x = sine(f, 48000, n);
        auto y = run(run(x, kA, kB), kB, kA);
        double db = 20 * std::log10(rmsMid(y) / rmsMid(x));
        double tol = f == 15000.0 ? 1.0 : 0.1;
        bool pass = std::fabs(db) <= tol;
        ok &= pass;
        std::printf("round trip %6.0f Hz: %+.4f dB (tol %.1f) %s\n", f, db, tol, pass ? "ok" : "FAIL");
    }
    {
        auto x = sine(20000, 48000, n);
        auto y = run(x, kA, kB);
        double db = 20 * std::log10(rmsMid(y) / rmsMid(x));
        bool pass = db <= -75;
        ok &= pass;
        std::printf("20 kHz 48k->D: %.2f dB %s\n", db, pass ? "ok" : "FAIL");
    }
    {
        auto x = sine(10000, D, size_t(4 * D));
        auto y = run(x, kB, kA);
        double tone = goertzel(y, 10000, 48000), img = goertzel(y, D - 10000, 48000);
        double db = 20 * std::log10(img / tone);
        bool pass = db <= -75;
        ok &= pass;
        std::printf("10 kHz D->48k image at %.2f Hz: %.2f dB %s\n", D - 10000, db, pass ? "ok" : "FAIL");
    }
    std::printf(ok ? "ALL OK\n" : "FAILURES\n");
    return ok ? 0 : 1;
}
