#pragma once
#include "results/measurements.hpp"
namespace pds {
struct TimeStatistics {
    double begin = 0, end = 0, integral = 0, mean = 0, rms = 0, standard_deviation = 0;
    size_t intervals = 0;
};
// Integrate the recorded analog polyline, or a zero-order hold for gate channels.
TimeStatistics time_statistics(const Result &, int channel, double begin, double end);
struct PowerEnergy {
    double begin = 0, end = 0, energy = 0, absorbed = 0, returned = 0, mean_power = 0;
    double minimum_power = 0, maximum_power = 0;
    size_t intervals = 0;
};
PowerEnergy power_energy(const Result &, int voltage, int current, double begin, double end,
                         bool reverse_current = false);
} // namespace pds
