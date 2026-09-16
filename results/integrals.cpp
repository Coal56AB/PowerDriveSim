#include "results/integrals.hpp"
#include <algorithm>
#include <array>
#include <cmath>

namespace pds {
namespace {
template <typename F> void segments(const Result &r, double &begin, double &end, F visit) {
    if (!std::isfinite(begin) || !std::isfinite(end) || end <= begin || r.samples.size() < 2)
        return;
    begin = std::max(begin, r.samples.front().time);
    end = std::min(end, r.samples.back().time);
    if (end <= begin)
        return;
    auto it = std::upper_bound(r.samples.begin(), r.samples.end(), begin,
                               [](double t, const Sample &sample) { return t < sample.time; });
    size_t first = it == r.samples.begin() ? 0 : static_cast<size_t>(it - r.samples.begin() - 1);
    for (size_t k = first; k + 1 < r.samples.size() && r.samples[k].time < end; ++k) {
        const double t0 = r.samples[k].time, t1 = r.samples[k + 1].time;
        if (!std::isfinite(t0) || !std::isfinite(t1) || t1 <= t0)
            throw Diagnostic("invalid_measurement_data", r.project_id,
                             "Recorded times must be finite and increasing");
        const double a = std::max(begin, t0), b = std::min(end, t1);
        if (b > a)
            visit(k, b - a, (a - t0) / (t1 - t0), (b - t0) / (t1 - t0));
    }
}
std::pair<long double, long double> endpoints(const Result &r, int channel, size_t k, double a, double b) {
    const double first = channel_value(r, k, channel);
    const double last =
        channel < static_cast<int>(r.channels.size()) ? channel_value(r, k + 1, channel) : first;
    if (!std::isfinite(first) || !std::isfinite(last))
        throw Diagnostic("invalid_measurement_data", result_channel(r, channel).object,
                         "Recorded values must be finite");
    const long double slope = static_cast<long double>(last) - first;
    return {first + a * slope, first + b * slope};
}
} // namespace
TimeStatistics time_statistics(const Result &r, int channel, double begin, double end) {
    (void)result_channel(r, channel);
    TimeStatistics result;
    long double integral = 0, weight = 0, mean = 0, variance_sum = 0;
    segments(r, begin, end, [&](size_t k, double duration, double a, double b) {
        const auto [v0, v1] = endpoints(r, channel, k, a, b);
        integral += duration * (v0 + v1) / 2;
        const auto segment_mean = (v0 + v1) / 2, delta = segment_mean - mean;
        const auto total = weight + duration;
        variance_sum += duration * (v1 - v0) * (v1 - v0) / 12 + delta * delta * weight * duration / total;
        mean += delta * duration / total;
        weight = total;
        ++result.intervals;
    });
    if (result.intervals) {
        result.begin = begin;
        result.end = end;
        result.integral = static_cast<double>(integral);
        result.mean = static_cast<double>(mean);
        result.standard_deviation = std::sqrt(static_cast<double>(variance_sum / weight));
        result.rms = std::hypot(result.mean, result.standard_deviation);
        if (!std::isfinite(result.integral) || !std::isfinite(result.rms))
            throw Diagnostic("measurement_overflow", r.project_id, "Signal integral exceeds numeric range");
    }
    return result;
}
PowerEnergy power_energy(const Result &r, int voltage, int current, double begin, double end, bool reverse) {
    if (result_channel(r, voltage).unit != "V" || result_channel(r, current).unit != "A")
        throw Diagnostic("measurement_units", r.project_id,
                         "Power requires a voltage channel and a current channel");
    PowerEnergy result;
    long double energy = 0, absorbed = 0, returned = 0;
    segments(r, begin, end, [&](size_t k, double duration, double left, double right) {
        const auto [u0, u1] = endpoints(r, voltage, k, left, right);
        auto [i0, i1] = endpoints(r, current, k, left, right);
        if (reverse) {
            i0 = -i0;
            i1 = -i1;
        }
        const long double du = u1 - u0, di = i1 - i0;
        const long double a = u0 * i0, b = u0 * di + i0 * du, c = du * di;
        auto power = [&](long double x) { return (c * x + b) * x + a; };
        auto primitive = [&](long double x) { return ((c / 3 * x + b / 2) * x + a) * x; };
        long double low = std::min(power(0), power(1)), high = std::max(power(0), power(1));
        if (c != 0) {
            const long double vertex = -b / (2 * c);
            if (vertex > 0 && vertex < 1) {
                low = std::min(low, power(vertex));
                high = std::max(high, power(vertex));
            }
        }
        if (!result.intervals) {
            result.minimum_power = static_cast<double>(low);
            result.maximum_power = static_cast<double>(high);
        } else {
            result.minimum_power = std::min(result.minimum_power, static_cast<double>(low));
            result.maximum_power = std::max(result.maximum_power, static_cast<double>(high));
        }
        std::array<long double, 4> cuts{0, 1};
        size_t count = 2;
        for (auto [value, slope] : {std::pair{u0, du}, std::pair{i0, di}})
            if (slope != 0) {
                const auto zero = -value / slope;
                if (zero > 0 && zero < 1)
                    cuts[count++] = zero;
            }
        std::sort(cuts.begin(), cuts.begin() + count);
        for (size_t j = 1; j < count; ++j) {
            const auto part = duration * (primitive(cuts[j]) - primitive(cuts[j - 1]));
            energy += part;
            if (power((cuts[j] + cuts[j - 1]) / 2) >= 0)
                absorbed += part;
            else
                returned -= part;
        }
        ++result.intervals;
    });
    if (result.intervals) {
        result.begin = begin;
        result.end = end;
        result.energy = static_cast<double>(energy);
        result.absorbed = static_cast<double>(absorbed);
        result.returned = static_cast<double>(returned);
        result.mean_power = static_cast<double>(energy / (end - begin));
        if (!std::isfinite(result.energy) || !std::isfinite(result.absorbed) ||
            !std::isfinite(result.returned) || !std::isfinite(result.minimum_power) ||
            !std::isfinite(result.maximum_power) || !std::isfinite(result.mean_power))
            throw Diagnostic("measurement_overflow", r.project_id, "Power integral exceeds numeric range");
    }
    return result;
}
} // namespace pds
