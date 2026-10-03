// Kaiser-windowed sinc prototype table for StreamResampler (resampler.h).
#include "resampler.h"

namespace {

// Modified Bessel function of the first kind, order 0: sum_k ((x/2)^k / k!)^2.
double besselI0(double x) {
    const double h = 0.5 * x;
    double term = 1.0, sum = 1.0;
    for (int k = 1; term >= 1e-12 * sum; k++) {
        const double r = h / k;
        term *= r * r;
        sum += term;
    }
    return sum;
}

}  // namespace

void initKernelTable(float* t) {
    const double beta = 0.1102 * (kKernelA - 8.7);
    const double i0Beta = besselI0(beta);
    const double W = kKernelHalfWidth;
    for (int i = 0; i < kKernelTableSize - 1; i++) {
        const double y = double(i) / kKernelP;
        if (y > W) {
            t[i] = 0.0f;
            continue;
        }
        const double u = 2.0 * kKernelFc * y;
        const double sinc = u == 0.0 ? 1.0 : std::sin(M_PI * u) / (M_PI * u);
        const double r = y / W;
        t[i] = float(2.0 * kKernelFc * sinc * besselI0(beta * std::sqrt(1.0 - r * r)) / i0Beta);
    }
    t[kKernelTableSize - 1] = 0.0f;
}
