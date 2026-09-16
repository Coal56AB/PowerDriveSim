#pragma once
#include "results/measurements.hpp"
#include <iosfwd>

namespace pds {
enum class SpectrumWindow { rectangular, hann, hamming, blackman };
struct SpectrumOptions {
    double begin = 0, end = 1;
    size_t samples = 4096;
    SpectrumWindow window = SpectrumWindow::hann;
    double fundamental = 0;
    unsigned harmonics = 40;
    bool whole_periods = true;
};
struct SpectrumBin {
    double frequency = 0, amplitude = 0, phase = 0; // Peak amplitude; cosine phase, radians.
};
struct Harmonic {
    unsigned order = 0;
    double frequency = 0, rms = 0, phase = 0;
};
struct Spectrum {
    Channel channel;
    double begin = 0, end = 0, sample_rate = 0, resolution = 0;
    double maximum_interpolation_gap = 0;
    size_t recorded_samples = 0;
    SpectrumWindow window = SpectrumWindow::hann;
    std::vector<SpectrumBin> bins;
    std::vector<Harmonic> harmonics;
    std::optional<double> thd; // Ratio, not percent. Excludes DC and Nyquist.
    bool coherent = false;
};
// Uniform point resampling of the recorded trace: analog linear, gate held.
// No anti-alias filter is applied; choose samples and original solver step accordingly.
Spectrum signal_spectrum(const Result &, int channel, const SpectrumOptions &);
void write_spectrum_csv(std::ostream &, const Spectrum &);
} // namespace pds
