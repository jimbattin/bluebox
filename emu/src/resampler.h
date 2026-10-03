// Allocation-free streaming band-limited resampler.
//
// One prototype kernel g(y), tabulated in units of the lower of the two rates,
// serves every direction: with s = min(r_in, r_out) / r_in the filter in input
// samples is h(x) = s * g(s * x), half-width kKernelHalfWidth / s. g is a
// Kaiser-windowed sinc with pass edge 0.46, stop edge 0.5 x the lower rate and
// an 80 dB stopband.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numeric>

constexpr double kKernelA = 80.0;   // stopband dB; beta = 0.1102 * (A - 8.7)
constexpr double kKernelFc = 0.48;  // cycles per min-rate sample (-6 dB)
// 0.5 * (A - 8) / (2.285 * 2 * pi * 0.04), in min-rate samples.
constexpr double kKernelHalfWidth = 62.686848482584594;
constexpr int kKernelP = 128;  // table entries per min-rate sample
constexpr int kKernelTableSize = int(kKernelHalfWidth * kKernelP) + 3;

// t[i] = g(i / kKernelP) for i in [0, kKernelTableSize - 2]; t[last] = 0.
void initKernelTable(float* t);

// Ch-channel resampler. Output k is the input signal evaluated at
// t_k = k * num / den input samples; it can be pulled once every input it needs
// has been pushed. Every tap steps through the prototype table by exactly
// kKernelP entries, so the interpolation fraction is constant per side of the
// output (or input) position and a tap is loads and FMAs only:
//  - upsampling (s = 1) gathers: for each output, sum the inputs within W;
//  - downsampling scatters (transposed form, table in output samples): each
//    input adds into the accumulators of the outputs within W of it.
// Cap bounds the frames in flight: pushed-but-unused inputs when gathering,
// started-but-unpulled outputs when scattering.
template <int Ch, size_t Cap> class StreamResampler {
    static_assert(Cap != 0 && (Cap & (Cap - 1)) == 0, "Cap must be a power of two");
    static_assert(Cap <= 0x40000000u, "Cap must fit the 32-bit ring index");
    static constexpr int P = kKernelP;
    static constexpr size_t kMask = Cap - 1;

public:
    // num / den = r_in / r_out. `kernel` comes from initKernelTable.
    void init(const float* kernel, uint32_t num, uint32_t den) {
        const uint32_t g = std::gcd(num, den);
        num_ = num / g;
        den_ = den / g;
        k_ = kernel;
        stepInt_ = num_ / den_;
        stepRem_ = num_ % den_;
        down_ = num_ > den_;
        const double s = down_ ? double(den_) / double(num_) : 1.0;
        w_ = kKernelHalfWidth / s;
        s_ = float(s);
        ip_ = 0;
        rem_ = 0;
        pushed_ = 0;
        uip_ = 0;
        urem_ = 0;
        pulled_ = 0;
        std::memset(ring_, 0, sizeof ring_);
    }

    void push(const float* frame) {
        if (down_)
            scatter(frame);
        else
            for (int c = 0; c < Ch; c++) {
                const size_t r = size_t(pushed_) & kMask;
                ring_[c][r] = ring_[c][r + Cap] = frame[c];  // mirrored: spans are contiguous
            }
        ++pushed_;
    }

    // Offsets relative to ip_ stay int: 64-bit <-> double is a libcall on Cortex-M7.
    bool ready() const { return ip_ + int(std::floor(frac() + w_)) <= pushed_ - 1; }

    void pull(float* frame) {
        if (down_) {
            const size_t r = size_t(pulled_) & kMask;
            for (int c = 0; c < Ch; c++) {
                frame[c] = s_ * ring_[c][r];
                ring_[c][r] = 0.0f;
            }
            ++pulled_;
        } else {
            gather(frame);
        }
        ip_ += stepInt_;
        rem_ += stepRem_;
        if (rem_ >= den_) {
            rem_ -= den_;
            ++ip_;
        }
    }

    double halfWidth() const { return w_; }  // in input samples

private:
    double frac() const { return double(rem_) / double(den_); }

    // Output at t = ip_ + x from inputs j in [t - W, t + W], j >= 0. With
    // a = x * P = ia + fa: input ip_ - m (m >= 0) is at table position
    // ia + P*m + fa, input ip_ + m (m >= 1) at (P*m - ia - 1) + (1 - fa).
    void gather(float* frame) const {
        const double x = frac();
        const double a = x * P;
        const int ia = int(a);
        const float fa = float(a - ia);
        const int lo = int(std::ceil(x - w_));
        const int mL = ip_ + lo >= 0 ? -lo : int(ip_);  // inputs before 0 are silent
        const int mR = int(std::floor(x + w_));
        const size_t start = size_t(ip_ - mL) & kMask;
        for (int c = 0; c < Ch; c++) {
            const float* xp = &ring_[c][start] + mL;  // input ip_
            float l0 = 0, l1 = 0, l0b = 0, l1b = 0;  // two chains each: FMA latency
            const float* kp = k_ + ia;
            int m = 0;
            for (; m + 1 <= mL; m += 2, kp += 2 * P) {
                l0 += xp[-m] * kp[0];
                l1 += xp[-m] * kp[1];
                l0b += xp[-m - 1] * kp[P];
                l1b += xp[-m - 1] * kp[P + 1];
            }
            if (m <= mL) {
                l0 += xp[-m] * kp[0];
                l1 += xp[-m] * kp[1];
            }
            float r0 = 0, r1 = 0, r0b = 0, r1b = 0;
            kp = k_ + (P - ia - 1);
            m = 1;
            for (; m + 1 <= mR; m += 2, kp += 2 * P) {
                r0 += xp[m] * kp[0];
                r1 += xp[m] * kp[1];
                r0b += xp[m + 1] * kp[P];
                r1b += xp[m + 1] * kp[P + 1];
            }
            if (m <= mR) {
                r0 += xp[m] * kp[0];
                r1 += xp[m] * kp[1];
            }
            l0 += l0b;
            l1 += l1b;
            r0 += r0b;
            r1 += r1b;
            frame[c] = s_ * (l0 + fa * (l1 - l0) + r0 + (1.0f - fa) * (r1 - r0));
        }
    }

    // Input pushed_ sits at u = uip_ + urem_ / num_ output samples. With
    // b = frac(u) * P = ib + fb: output uip_ - m (m >= 0) is at table position
    // ib + P*m + fb, output uip_ + m (m >= 1) at (P*m - ib - 1) + (1 - fb).
    // The fraction is constant per side, so x * lerp(t0, t1) folds into two
    // per-input weights: x*(1-f)*t0 + x*f*t1. Outputs already pulled are skipped.
    void scatter(const float* frame) {
        const double u = double(urem_) / double(num_);
        const double b = u * P;
        const int ib = int(b);
        const float fb = float(b - ib);
        const float gb = 1.0f - fb;
        const int mR = int(std::floor(u + kKernelHalfWidth));
        int mL = -int(std::ceil(u - kKernelHalfWidth));
        if (uip_ - mL < pulled_) mL = int(uip_ - pulled_);
        const int mR0 = uip_ + 1 >= pulled_ ? 1 : int(pulled_ - uip_);
        // Ring slots wrap mod Cap, which divides 2^32, so the low 32 bits of uip_
        // suffice and the per-tap index arithmetic stays 32-bit.
        const uint32_t base = uint32_t(uint64_t(uip_));
        const float* const k = k_;
        for (int c = 0; c < Ch; c++) {
            float* const acc = ring_[c];  // a local copy: stores to it cannot alias frame
            const float x = frame[c];
            const float lo0 = x * gb, lo1 = x * fb;  // left side:  (1-fb)*t0 + fb*t1
            const float hi0 = x * fb, hi1 = x * gb;  // right side: fb*t0 + (1-fb)*t1
            // Two taps per iteration, both loaded before either is stored, and the
            // weight formed apart from the accumulator: the M7 is in-order, so
            // the load-add-store chain through each accumulator is the cost.
            const float* kp = k + ib;
            int m = 0;
            for (; m + 1 <= mL; m += 2, kp += 2 * P) {
                float& a0 = acc[size_t(base - uint32_t(m)) & kMask];
                float& a1 = acc[size_t(base - uint32_t(m + 1)) & kMask];
                const float n0 = a0 + (lo0 * kp[0] + lo1 * kp[1]);
                const float n1 = a1 + (lo0 * kp[P] + lo1 * kp[P + 1]);
                a0 = n0;
                a1 = n1;
            }
            if (m <= mL) {
                float& a0 = acc[size_t(base - uint32_t(m)) & kMask];
                a0 = a0 + (lo0 * kp[0] + lo1 * kp[1]);
            }
            kp = k + (P * mR0 - ib - 1);
            m = mR0;
            for (; m + 1 <= mR; m += 2, kp += 2 * P) {
                float& a0 = acc[size_t(base + uint32_t(m)) & kMask];
                float& a1 = acc[size_t(base + uint32_t(m + 1)) & kMask];
                const float n0 = a0 + (hi0 * kp[0] + hi1 * kp[1]);
                const float n1 = a1 + (hi0 * kp[P] + hi1 * kp[P + 1]);
                a0 = n0;
                a1 = n1;
            }
            if (m <= mR) {
                float& a0 = acc[size_t(base + uint32_t(m)) & kMask];
                a0 = a0 + (hi0 * kp[0] + hi1 * kp[1]);
            }
        }
        urem_ += den_;
        if (urem_ >= num_) {
            urem_ -= num_;
            ++uip_;
        }
    }

    const float* k_ = nullptr;
    uint32_t num_ = 1, den_ = 1, stepInt_ = 1, stepRem_ = 0;
    bool down_ = false;
    double w_ = kKernelHalfWidth;  // input samples
    float s_ = 1.0f;
    int64_t ip_ = 0;     // integer part of t
    uint32_t rem_ = 0;   // t = ip_ + rem_ / den_
    int64_t pushed_ = 0;
    int64_t uip_ = 0;    // scatter: next input's position in output samples,
    uint32_t urem_ = 0;  //          uip_ + urem_ / num_
    int64_t pulled_ = 0;
    // Gather: mirrored input ring (sample j at j & kMask and at that + Cap).
    // Scatter: output accumulators in [0, Cap).
    float ring_[Ch][2 * Cap];
};
