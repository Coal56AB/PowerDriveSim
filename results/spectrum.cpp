#include "results/spectrum.hpp"
#include <algorithm>
#include <cmath>
#include <complex>
#include <iomanip>
#include <limits>
#include <locale>
#include <numbers>
#include <ostream>
#include <sstream>

namespace pds {
namespace {
constexpr double pi = std::numbers::pi;
using Complex = std::complex<double>;
void fft(std::vector<Complex> &x) {
    const size_t n = x.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(x[i], x[j]);
    }
    for (size_t length = 2; length <= n; length <<= 1) {
        const auto root = std::polar(1., -2 * pi / double(length));
        for (size_t base = 0; base < n; base += length) {
            Complex factor = 1;
            for (size_t k = 0; k < length / 2; ++k) {
                const auto left = x[base + k], right = x[base + k + length / 2] * factor;
                x[base + k] = left + right;
                x[base + k + length / 2] = left - right;
                factor *= root;
            }
        }
    }
}
double window_weight(SpectrumWindow window, size_t index, size_t n) {
    const double a = 2 * pi * double(index) / double(n);
    switch (window) {
    case SpectrumWindow::rectangular:
        return 1;
    case SpectrumWindow::hann:
        return .5 - .5 * std::cos(a);
    case SpectrumWindow::hamming:
        return .54 - .46 * std::cos(a);
    case SpectrumWindow::blackman:
        return .42 - .5 * std::cos(a) + .08 * std::cos(2 * a);
    }
    throw Diagnostic("invalid_spectrum_window", "", "Unknown spectral window");
}
} // namespace

Spectrum signal_spectrum(const Result &r, int channel, const SpectrumOptions &o) {
    Spectrum out;
    out.channel = result_channel(r, channel);
    if (o.samples < 32 || o.samples > 1048576 || (o.samples & (o.samples - 1)) || !std::isfinite(o.begin) ||
        !std::isfinite(o.end) || o.end <= o.begin || !std::isfinite(o.fundamental) || o.fundamental < 0 ||
        o.harmonics < 2 || o.harmonics > 1000)
        throw Diagnostic("invalid_spectrum_options", out.channel.object,
                         "Invalid FFT size, interval or harmonics");
    if (r.samples.size() < 2)
        throw Diagnostic("insufficient_spectrum_data", out.channel.object,
                         "FFT requires at least two recorded samples");
    out.begin = std::max(o.begin, r.samples.front().time);
    out.end = std::min(o.end, r.samples.back().time);
    if (out.end <= out.begin)
        throw Diagnostic("insufficient_spectrum_data", out.channel.object,
                         "FFT interval is outside recorded data");
    double periods = (out.end - out.begin) * o.fundamental;
    if (!std::isfinite(out.end - out.begin) || !std::isfinite(periods))
        throw Diagnostic("invalid_spectrum_options", out.channel.object,
                         "FFT interval exceeds numeric range");
    if (o.fundamental > 0 && o.whole_periods) {
        periods = std::floor(periods + 1e-10 * std::max(1., periods));
        if (periods < 1)
            throw Diagnostic("insufficient_spectrum_periods", out.channel.object,
                             "Select at least one fundamental period");
        out.end = std::min(out.end, out.begin + periods / o.fundamental);
    }
    const double duration = out.end - out.begin;
    out.sample_rate = double(o.samples) / duration;
    out.resolution = 1 / duration;
    if (!std::isfinite(out.sample_rate) || out.begin + duration / double(o.samples) == out.begin)
        throw Diagnostic("invalid_spectrum_options", out.channel.object,
                         "FFT sampling interval is too small");
    if (o.fundamental >= out.sample_rate / 2)
        throw Diagnostic("spectrum_nyquist", out.channel.object,
                         "Fundamental must be below the FFT Nyquist frequency");
    out.window = o.window;
    auto first = std::lower_bound(r.samples.begin(), r.samples.end(), out.begin,
                                  [](const Sample &s, double t) { return s.time < t; });
    auto last = std::upper_bound(first, r.samples.end(), out.end,
                                 [](double t, const Sample &s) { return t < s.time; });
    out.recorded_samples = size_t(last - first);
    std::vector<Complex> plain(o.samples), windowed(o.samples);
    double gain = 0, maximum = 0;
    for (size_t k = 0; k < o.samples; ++k) {
        const double t = out.begin + duration * (double(k) / double(o.samples));
        auto next = std::upper_bound(r.samples.begin(), r.samples.end(), t,
                                     [](double time, const Sample &s) { return time < s.time; });
        const size_t right = std::min(size_t(next - r.samples.begin()), r.samples.size() - 1);
        const size_t left = right ? right - 1 : 0;
        const double gap = r.samples[right].time - r.samples[left].time;
        if (!(gap > 0) || !std::isfinite(gap))
            throw Diagnostic("invalid_measurement_data", out.channel.object, "Recorded times must increase");
        const double a = channel_value(r, left, channel), b = channel_value(r, right, channel);
        if (!std::isfinite(a) || !std::isfinite(b))
            throw Diagnostic("invalid_measurement_data", out.channel.object,
                             "Recorded values must be finite");
        const double v =
            channel < int(r.channels.size()) ? std::lerp(a, b, (t - r.samples[left].time) / gap) : a;
        out.maximum_interpolation_gap = std::max(out.maximum_interpolation_gap, gap);
        const double weight = window_weight(o.window, k, o.samples);
        gain += weight;
        // Pre-normalization avoids overflow in butterflies for large finite input.
        plain[k] = v / double(o.samples);
        windowed[k] = plain[k] * weight;
        maximum = std::max(maximum, std::abs(v));
    }
    gain /= double(o.samples);
    fft(windowed);
    out.bins.reserve(o.samples / 2 + 1);
    for (size_t k = 0; k <= o.samples / 2; ++k) {
        const double factor = k && k < o.samples / 2 ? 2 : 1;
        const double amplitude = factor * std::abs(windowed[k]) / gain;
        if (!std::isfinite(amplitude))
            throw Diagnostic("measurement_overflow", out.channel.object, "Spectrum exceeds numeric range");
        out.bins.push_back({k * out.resolution, amplitude, std::arg(windowed[k])});
    }
    periods = duration * o.fundamental;
    const double cycle_count = std::round(periods);
    out.coherent = o.fundamental > 0 && cycle_count >= 1 &&
                   std::abs(periods - cycle_count) <= 1e-8 * std::max(1., periods);
    if (out.coherent) {
        fft(plain); // Unwindowed coherent harmonics, independent of display window.
        const size_t stride = size_t(cycle_count);
        double distortion = 0;
        for (unsigned h = 1; h <= o.harmonics && size_t(h) * stride < o.samples / 2; ++h) {
            const size_t k = size_t(h) * stride;
            const double rms = std::sqrt(2.) * std::abs(plain[k]);
            out.harmonics.push_back({h, k * out.resolution, rms, std::arg(plain[k])});
            if (h > 1)
                distortion = std::hypot(distortion, rms);
        }
        if (out.harmonics.size() >= 2 && out.harmonics.front().rms > maximum * 1e-12)
            out.thd = distortion / out.harmonics.front().rms;
    }
    return out;
}

void write_spectrum_csv(std::ostream &out, const Spectrum &s) {
    // Format in an independent stream so caller locale and precision stay untouched.
    std::ostringstream data;
    data.imbue(std::locale::classic());
    data << std::setprecision(17) << "# begin_s=" << s.begin << ",end_s=" << s.end
         << ",sample_rate_hz=" << s.sample_rate << ",resolution_hz=" << s.resolution << '\n';
    data << "# amplitude_unit=" << std::quoted(s.channel.unit) << ",window=" << int(s.window) << '\n';
    data << "frequency_Hz,peak_amplitude,phase_rad\n";
    for (const auto &b : s.bins)
        data << b.frequency << ',' << b.amplitude << ',' << b.phase << '\n';
    data << "\norder,frequency_Hz,rms,phase_rad\n";
    for (const auto &h : s.harmonics)
        data << h.order << ',' << h.frequency << ',' << h.rms << ',' << h.phase << '\n';
    if (s.thd)
        data << "# thd_ratio=" << *s.thd << '\n';
    out << data.str();
}
} // namespace pds
