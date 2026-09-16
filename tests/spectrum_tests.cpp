#include "results/spectrum.hpp"
#include <chrono>
#include <cmath>
#include <complex>
#include <iostream>
#include <numbers>
#include <sstream>
using namespace pds;
namespace {
constexpr double pi = std::numbers::pi;
void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
void near(double actual, double expected, double tolerance = 1e-10) {
    if (std::abs(actual - expected) > tolerance) {
        std::cerr << actual << " != " << expected << '\n';
        throw std::runtime_error("Spectrum mismatch");
    }
}
Result trace(size_t n, bool irregular = false) {
    Result r;
    r.channels = {{"u", "Voltage", "V"}};
    for (size_t i = 0; i <= n; ++i) {
        double t = double(i) / double(n);
        if (irregular && i && i < n)
            t += .2 * std::sin(double(i)) / double(n);
        r.samples.push_back({t,
                             {3 + 2 * std::cos(2 * pi * 8 * t + .3) + .2 * std::cos(2 * pi * 24 * t - .2) +
                              .1 * std::sin(2 * pi * 40 * t)},
                             {}});
    }
    return r;
}
} // namespace
int main() try {
    auto r = trace(4096);
    SpectrumOptions o;
    o.fundamental = 8;
    for (auto window : {SpectrumWindow::rectangular, SpectrumWindow::hann, SpectrumWindow::hamming,
                        SpectrumWindow::blackman}) {
        o.window = window;
        auto s = signal_spectrum(r, 0, o);
        near(s.bins[0].amplitude, 3);
        near(s.bins[8].amplitude, 2);
        near(s.bins[8].phase, .3);
        near(s.bins[24].amplitude, .2);
        near(s.bins[40].amplitude, .1);
        check(s.coherent && s.thd && s.harmonics.size() == 40, "Harmonic selection");
        near(*s.thd, std::sqrt(.2 * .2 + .1 * .1) / 2);
        near(s.harmonics[0].rms, std::sqrt(2.));
    }
    // Independent O(N²) DFT oracle: arbitrary deterministic data, all complex bins.
    Result noise;
    noise.channels = r.channels;
    o.samples = 32;
    o.fundamental = 0;
    o.window = SpectrumWindow::rectangular;
    for (int k = 0; k <= 32; ++k)
        noise.samples.push_back({k / 32., {std::sin(k * 1.234) + (k % 7) * .31}, {}});
    auto n = signal_spectrum(noise, 0, o);
    for (size_t k = 0; k < n.bins.size(); ++k) {
        std::complex<double> expected;
        for (size_t j = 0; j < 32; ++j)
            expected += noise.samples[j].values[0] * std::polar(1., -2 * pi * double(k * j) / 32);
        expected /= 32;
        if (k && k < 16)
            expected *= 2;
        near(n.bins[k].amplitude, std::abs(expected));
        near(std::abs(std::polar(n.bins[k].amplitude, n.bins[k].phase) - expected), 0);
    }
    // Irregular source samples must be interpolated, not mistaken for uniform samples.
    o.samples = 4096;
    o.fundamental = 8;
    auto irregular = signal_spectrum(trace(65536, true), 0, o);
    near(irregular.bins[8].amplitude, 2, 2e-7);
    near(*irregular.thd, std::sqrt(.0125), 2e-7);
    o.begin = .03;
    o.end = .98;
    auto trim = signal_spectrum(r, 0, o);
    near(trim.end - trim.begin, .875);
    check(trim.coherent && trim.thd, "Whole periods");
    o.whole_periods = false;
    auto fractional = signal_spectrum(r, 0, o);
    check(!fractional.coherent && !fractional.thd && fractional.harmonics.empty(), "Fractional-period THD");
    o = {};
    o.fundamental = 8;
    Result constant = r;
    for (auto &sample : constant.samples)
        sample.values[0] = 3;
    check(!signal_spectrum(constant, 0, o).thd, "THD of DC must be unavailable");
    Result digital;
    digital.gate_objects = {"g"};
    digital.samples = {{0, {}, {true}}, {.5, {}, {false}}, {1, {}, {true}}};
    o.fundamental = 1;
    o.window = SpectrumWindow::rectangular;
    auto square = signal_spectrum(digital, 0, o);
    near(square.bins[0].amplitude, .5);
    near(square.bins[1].amplitude, 2 / pi, 1e-6);
    near(square.bins[2].amplitude, 0);
    // Nyquist is not doubled and does not participate in THD.
    auto nyquist = noise;
    for (size_t k = 0; k < nyquist.samples.size(); ++k)
        nyquist.samples[k].values[0] = k % 2 ? -1 : 1;
    o.samples = 32;
    o.fundamental = 0;
    near(signal_spectrum(nyquist, 0, o).bins.back().amplitude, 1);
    for (int which = 0; which < 4; ++which) {
        auto bad = o;
        if (which == 0)
            bad.samples = 100;
        if (which == 1)
            bad.end = bad.begin;
        if (which == 2)
            bad.fundamental = 16;
        if (which == 3) {
            bad.fundamental = .5;
            bad.whole_periods = true;
        }
        bool rejected = false;
        try {
            signal_spectrum(r, 0, bad);
        } catch (const Diagnostic &) {
            rejected = true;
        }
        check(rejected, "Invalid FFT options accepted");
    }
    std::ostringstream csv;
    csv.precision(3);
    write_spectrum_csv(csv, square);
    check(csv.precision() == 3 &&
              csv.str().find("frequency_Hz,peak_amplitude,phase_rad") != std::string::npos &&
              csv.str().find("# thd_ratio=") != std::string::npos,
          "Spectrum CSV");
    o = {};
    o.samples = 65536;
    o.fundamental = 8;
    const auto start = std::chrono::steady_clock::now();
    auto large = signal_spectrum(r, 0, o);
    std::cout << "65536-point FFT: "
              << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()
              << " ms\n";
    near(large.bins[8].amplitude, 2, 3e-5);
    std::cout << "PASS FFT/DFT, windows, phases, harmonics, THD, resampling and CSV\n";
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
