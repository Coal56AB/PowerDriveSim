#pragma once
#include "core/solver/reference/reference.hpp"
#include <optional>
namespace pds {
struct CursorValue {
    double time = 0, value = 0;
    std::string key, name, unit;
};
double channel_value(const Result &result, size_t sample, int channel);
Channel result_channel(const Result &result, int channel);
std::optional<CursorValue> cursor_value(const Result &result, int channel, double time);
struct CursorMath {
    double dt = 0, interval = 0;
    std::optional<double> frequency, dy, slope;
};
CursorMath compare_cursors(const CursorValue &a, const CursorValue &b);
struct SignalStatistics {
    size_t count = 0;
    double minimum = 0, maximum = 0, minimum_time = 0, maximum_time = 0, mean = 0, median = 0, rms = 0,
           mean_square = 0, standard_deviation = 0;
};
SignalStatistics signal_statistics(const Result &, int channel, double begin, double end);
struct SignalPeak {
    double time = 0, value = 0;
};
std::vector<SignalPeak> signal_peaks(const Result &, int channel, double begin, double end, double threshold,
                                     double excursion, double distance, size_t limit);
struct SignalEdge {
    double time = 0;
    bool rising = false;
};
std::vector<SignalEdge> signal_edges(const Result &, int channel, double begin, double end, double level);
struct PulseMeasurements {
    double low = 0, high = 0, overshoot = 0, undershoot = 0;
    size_t cycles = 0;
    std::optional<double> period, frequency, duty, high_width, low_width, rise_time, fall_time;
};
PulseMeasurements pulse_measurements(const Result &, int channel, double begin, double end,
                                     std::optional<std::pair<double, double>> levels = {},
                                     double lower_reference = .1, double upper_reference = .9);
} // namespace pds
