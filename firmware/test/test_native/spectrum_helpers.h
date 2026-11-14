// Host-side spectrum measurement for native tests (never runs on the device).
#pragma once

#include <cmath>
#include <complex>
#include <vector>

namespace spectrum {

constexpr double kFs = 44100.0;

inline void fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * M_PI / static_cast<double>(len);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k];
                const auto v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// Averaged Hann-windowed periodogram of the left channel of any renderer with
// a render(float* interleaved, size_t frames) method.
template <typename R>
std::vector<double> psd(R& r, int segments, int n = 4096) {
    std::vector<float> buf(n * 2);
    std::vector<double> win(n), acc(n / 2 + 1, 0.0);
    for (int i = 0; i < n; ++i) win[i] = 0.5 - 0.5 * std::cos(2.0 * M_PI * i / n);
    std::vector<std::complex<double>> x(n);
    r.render(buf.data(), n);  // discard start-up transient
    for (int s = 0; s < segments; ++s) {
        r.render(buf.data(), n);
        for (int i = 0; i < n; ++i) x[i] = {buf[2 * i] * win[i], 0.0};
        fft(x);
        for (int k = 0; k <= n / 2; ++k) acc[k] += std::norm(x[k]);
    }
    return acc;
}

inline double bandMean(const std::vector<double>& p, double f0, double f1) {
    const int n = static_cast<int>((p.size() - 1) * 2);
    const double df = kFs / n;
    const int k0 = static_cast<int>(std::ceil(f0 / df));
    const int k1 = static_cast<int>(std::floor(f1 / df));
    double sum = 0.0;
    for (int k = k0; k < k1; ++k) sum += p[k];
    return sum / (k1 - k0);
}

inline double db(double ratio) { return 10.0 * std::log10(ratio); }

}  // namespace spectrum
