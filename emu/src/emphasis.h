// 224X converter emphasis. AIN card (224X service manual §3.9): a shelving
// pre-emphasis ahead of the ADC, time constants 50 us (zero) and 12.5 us (pole):
// +1.3 dB at 2 kHz, +7.2 dB at 8 kHz, +12 dB asymptote. AOUT card (§3.10):
// de-emphasis after the DAC (stages U13, U21); its time constants are not given,
// so it is modelled as the exact inverse [INFERENCE]. (The original 224's A-IN,
// 8 kHz bandwidth, used a different shelf: +2.6 dB at 2 kHz, +8.15 dB at 8 kHz.)
#pragma once
#include <algorithm>
#include <cmath>

constexpr double kEmphasisTauZero = 50e-6;   // s
constexpr double kEmphasisTauPole = 12.5e-6; // s

// H(s) = (1 + s*tauZero) / (1 + s*tauPole), bilinear transform with the frequency
// warping matched at min(8 kHz, fs/4). Swapping the time constants gives the exact
// inverse filter. Allocation-free; init() also clears the state.
class Shelf1 {
public:
    void init(double fs, double tauZero, double tauPole) {
        constexpr double kPi = 3.14159265358979323846;
        const double wm = 2.0 * kPi * std::min(8000.0, fs / 4.0);
        const double K = wm / std::tan(wm / (2.0 * fs));
        const double den = 1.0 + tauPole * K;
        b0_ = float((1.0 + tauZero * K) / den);
        b1_ = float((1.0 - tauZero * K) / den);
        a1_ = float((1.0 - tauPole * K) / den);
        x1_ = y1_ = 0.0f;
    }
    float process(float x) {
        const float y = b0_ * x + b1_ * x1_ - a1_ * y1_;
        x1_ = x;
        y1_ = y;
        return y;
    }

private:
    float b0_ = 1.0f, b1_ = 0.0f, a1_ = 0.0f, x1_ = 0.0f, y1_ = 0.0f;
};
