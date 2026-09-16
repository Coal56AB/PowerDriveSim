#include "results/integrals.hpp"
#include <cmath>
#include <iostream>
#include <limits>
using namespace pds;
namespace {
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
void near(double actual, double expected, double tolerance = 1e-10) {
    check(std::abs(actual - expected) < tolerance, "Time integral mismatch");
}
} // namespace
int main() try {
    Result r;
    r.channels = {{"voltage", "u", "V"}, {"current", "i", "A"}};
    for (double t : {0., .1, .9, 1.1, 2.4, 3.})
        r.samples.push_back({t, {t, 2 * t - 2}, {}});
    auto s = time_statistics(r, 0, .2, 2.7);
    near(s.begin, .2);
    near(s.end, 2.7);
    near(s.mean, 1.45);
    near(s.rms, std::sqrt((.2 * .2 + .2 * 2.7 + 2.7 * 2.7) / 3));
    near(s.integral, 1.45 * 2.5);
    near(s.standard_deviation, 2.5 / std::sqrt(12.));
    auto p = power_energy(r, 0, 1, 0, 3);
    near(p.energy, 9);
    near(p.returned, 1. / 3);
    near(p.absorbed, 9 + 1. / 3);
    near(p.mean_power, 3);
    near(p.minimum_power, -.5);
    near(p.maximum_power, 12);
    auto reversed = power_energy(r, 0, 1, 0, 3, true);
    near(reversed.energy, -p.energy);
    near(reversed.absorbed, p.returned);
    near(reversed.returned, p.absorbed);
    Result sparse = r;
    sparse.samples = {r.samples.front(), r.samples.back()};
    near(power_energy(sparse, 0, 1, -100, 100).energy, 9);
    near(time_statistics(sparse, 0, .2, 2.7).rms, s.rms);
    check(!time_statistics(r, 0, 2, 1).intervals && !power_energy(r, 0, 1, 10, 20).intervals, "Empty range");
    auto clipped = power_energy(r, 0, 1, .2, 2.7);
    auto primitive = [](double t) { return 2 * t * t * t / 3 - t * t; };
    near(clipped.energy, primitive(2.7) - primitive(.2));
    Result digital;
    digital.gate_objects = {"g"};
    digital.samples = {{0, {}, {true}}, {.1, {}, {true}}, {.2, {}, {false}}, {1, {}, {false}}};
    auto gate = time_statistics(digital, 0, 0, 1);
    near(gate.mean, .2);
    near(gate.rms, std::sqrt(.2));
    near(gate.standard_deviation, .4);
    Result offset;
    offset.channels = {{"u", "u", "V"}};
    for (double t : {0., 1., 3.})
        offset.samples.push_back({t, {1e12 + t}, {}});
    near(time_statistics(offset, 0, 0, 3).standard_deviation, std::sqrt(.75));
    try {
        power_energy(r, 1, 0, 0, 3);
        check(false, "Unit mismatch accepted");
    } catch (const Diagnostic &e) {
        check(e.code == "measurement_units", "Wrong unit diagnostic");
    }
    r.samples[2].values[0] = std::numeric_limits<double>::quiet_NaN();
    try {
        time_statistics(r, 0, 0, 3);
        check(false, "Nonfinite signal accepted");
    } catch (const Diagnostic &e) {
        check(e.code == "invalid_measurement_data", "Wrong signal diagnostic");
    }
    std::cout << "PASS irregular time statistics, signed energy, extrema, clipping and digital holds\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
