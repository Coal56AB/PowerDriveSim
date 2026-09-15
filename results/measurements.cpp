#include "results/measurements.hpp"
#include <algorithm>
#include <cmath>
namespace pds {
double channel_value(const Result &r, size_t i, int channel) {
    const auto &s = r.samples.at(i);
    if (channel < static_cast<int>(r.channels.size()))
        return s.values.at(channel);
    return s.gates.at(channel - r.channels.size()) ? 1. : 0.;
}
Channel result_channel(const Result &r, int channel) {
    if (channel < static_cast<int>(r.channels.size()))
        return r.channels.at(channel);
    auto id = r.gate_objects.at(channel - r.channels.size());
    return {"gate/" + id, "gate:" + id, "bool"};
}
std::optional<CursorValue> cursor_value(const Result &r, int channel, double time) {
    if (r.samples.empty() || channel < 0 ||
        channel >= static_cast<int>(r.channels.size() + r.gate_objects.size()) || !std::isfinite(time) ||
        time < 0)
        return {};
    auto it = std::lower_bound(r.samples.begin(), r.samples.end(), time,
                               [](const Sample &s, double t) { return s.time < t; });
    size_t i = it == r.samples.end() ? r.samples.size() - 1 : static_cast<size_t>(it - r.samples.begin());
    if (i && std::abs(r.samples[i - 1].time - time) <= std::abs(r.samples[i].time - time))
        --i;
    const auto c = result_channel(r, channel);
    return CursorValue{r.samples[i].time, channel_value(r, i, channel), c.object, c.name, c.unit};
}
CursorMath compare_cursors(const CursorValue &a, const CursorValue &b) {
    CursorMath m;
    m.dt = b.time - a.time;
    m.interval = std::abs(m.dt);
    if (m.interval > 0)
        m.frequency = 1 / m.interval;
    if (a.unit == b.unit) {
        m.dy = b.value - a.value;
        if (m.dt != 0)
            m.slope = *m.dy / m.dt;
    }
    return m;
}

namespace {
std::pair<size_t, size_t> interval(const Result &r, double begin, double end) {
    if (!std::isfinite(begin) || !std::isfinite(end) || begin > end)
        return {0, 0};
    auto a = std::lower_bound(r.samples.begin(), r.samples.end(), begin,
                              [](const Sample &s, double t) { return s.time < t; });
    auto b = std::upper_bound(r.samples.begin(), r.samples.end(), end,
                              [](double t, const Sample &s) { return t < s.time; });
    return {a - r.samples.begin(), b - r.samples.begin()};
}
} // namespace
SignalStatistics signal_statistics(const Result &r, int channel, double begin, double end) {
    SignalStatistics result;
    auto [a, b] = interval(r, begin, end);
    std::vector<double> values;
    values.reserve(b - a);
    long double mean = 0, m2 = 0, squares = 0;
    for (size_t i = a; i < b; ++i) {
        double v = channel_value(r, i, channel);
        if (!std::isfinite(v))
            continue;
        if (result.count == 0 || v < result.minimum) {
            result.minimum = v;
            result.minimum_time = r.samples[i].time;
        }
        if (result.count == 0 || v > result.maximum) {
            result.maximum = v;
            result.maximum_time = r.samples[i].time;
        }
        ++result.count;
        const long double delta = v - mean;
        mean += delta / result.count;
        m2 += delta * (v - mean);
        squares += static_cast<long double>(v) * v;
        values.push_back(v);
    }
    if (!result.count)
        return result;
    result.mean = static_cast<double>(mean);
    result.mean_square = static_cast<double>(squares / result.count);
    result.rms = std::sqrt(result.mean_square);
    result.standard_deviation = std::sqrt(static_cast<double>(m2 / result.count));
    auto middle = values.begin() + values.size() / 2;
    std::nth_element(values.begin(), middle, values.end());
    result.median = *middle;
    if (values.size() % 2 == 0)
        result.median = (*std::max_element(values.begin(), middle) + *middle) / 2;
    return result;
}
std::vector<SignalPeak> signal_peaks(const Result &r, int channel, double begin, double end, double threshold,
                                     double excursion, double distance, size_t limit) {
    auto [a, b] = interval(r, begin, end);
    std::vector<SignalPeak> candidates, peaks;
    if (b - a < 3 || limit == 0)
        return peaks;
    for (size_t i = a + 1; i + 1 < b; ++i) {
        double v = channel_value(r, i, channel);
        if (!(v >= threshold && v > channel_value(r, i - 1, channel)))
            continue;
        size_t last = i;
        while (last + 1 < b && channel_value(r, last + 1, channel) == v)
            ++last;
        if (last + 1 < b && channel_value(r, last + 1, channel) < v) {
            size_t left = i - 1, right = last + 1;
            while (left > a && channel_value(r, left - 1, channel) <= channel_value(r, left, channel))
                --left;
            while (right + 1 < b && channel_value(r, right + 1, channel) <= channel_value(r, right, channel))
                ++right;
            if (v - std::max(channel_value(r, left, channel), channel_value(r, right, channel)) >= excursion)
                candidates.push_back({(r.samples[i].time + r.samples[last].time) / 2, v});
        }
        i = last;
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](auto a, auto b) { return a.value > b.value; });
    for (auto p : candidates) {
        if (std::none_of(peaks.begin(), peaks.end(),
                         [&](auto other) { return std::abs(p.time - other.time) < distance; }))
            peaks.push_back(p);
        if (peaks.size() >= limit)
            break;
    }
    return peaks;
}
std::vector<SignalEdge> signal_edges(const Result &r, int channel, double begin, double end, double level) {
    std::vector<SignalEdge> edges;
    auto [a, b] = interval(r, begin, end);
    if (a > 0)
        --a;
    for (size_t i = a + 1; i < b; ++i) {
        double v0 = channel_value(r, i - 1, channel), v1 = channel_value(r, i, channel);
        bool rise = v0 < level && v1 >= level, fall = v0 >= level && v1 < level;
        if (!rise && !fall)
            continue;
        double t = r.samples[i].time;
        if (channel < static_cast<int>(r.channels.size()))
            t = r.samples[i - 1].time + (level - v0) / (v1 - v0) * (t - r.samples[i - 1].time);
        if (t >= begin && t <= end)
            edges.push_back({t, rise});
    }
    return edges;
}
PulseMeasurements pulse_measurements(const Result &r, int channel, double begin, double end,
                                     std::optional<std::pair<double, double>> levels, double lower_reference,
                                     double upper_reference) {
    PulseMeasurements m;
    auto stats = signal_statistics(r, channel, begin, end);
    if (!stats.count)
        return m;
    m.low = levels ? levels->first : stats.minimum;
    m.high = levels ? levels->second : stats.maximum;
    if (!levels && m.high > m.low) {
        auto [a, b] = interval(r, begin, end);
        for (int pass = 0; pass < 8; ++pass) {
            long double low = 0, high = 0;
            size_t nl = 0, nh = 0;
            double mid = (m.low + m.high) / 2;
            for (size_t i = a; i < b; ++i) {
                double v = channel_value(r, i, channel);
                if (v < mid) {
                    low += v;
                    ++nl;
                } else {
                    high += v;
                    ++nh;
                }
            }
            if (!nl || !nh)
                break;
            m.low = static_cast<double>(low / nl);
            m.high = static_cast<double>(high / nh);
        }
    }
    if (!(m.high > m.low) || lower_reference < 0 || upper_reference > 1 || lower_reference >= upper_reference)
        return m;
    const double amplitude = m.high - m.low;
    m.overshoot = std::max(0., (stats.maximum - m.high) / amplitude * 100);
    m.undershoot = std::max(0., (m.low - stats.minimum) / amplitude * 100);
    auto edges = signal_edges(r, channel, begin, end, (m.low + m.high) / 2),
         lower = signal_edges(r, channel, begin, end, m.low + amplitude * lower_reference),
         upper = signal_edges(r, channel, begin, end, m.low + amplitude * upper_reference);
    double periods = 0, high_width = 0, low_width = 0;
    size_t cycles = 0;
    for (size_t i = 0; i + 2 < edges.size(); ++i)
        if (edges[i].rising == edges.front().rising && edges[i].rising != edges[i + 1].rising &&
            edges[i + 2].rising == edges[i].rising) {
            double t = edges[i + 2].time - edges[i].time;
            if (t > 0) {
                periods += t;
                high_width += edges[i].rising ? edges[i + 1].time - edges[i].time
                                              : edges[i + 2].time - edges[i + 1].time;
                low_width += edges[i].rising ? edges[i + 2].time - edges[i + 1].time
                                             : edges[i + 1].time - edges[i].time;
                ++cycles;
            }
        }
    m.cycles = cycles;
    if (cycles) {
        m.period = periods / cycles;
        m.frequency = 1 / *m.period;
        m.high_width = high_width / cycles;
        m.low_width = low_width / cycles;
        m.duty = high_width / periods * 100;
    }
    auto transitions = [&](bool rising) -> std::optional<double> {
        const auto &start = rising ? lower : upper;
        const auto &finish = rising ? upper : lower;
        double sum = 0;
        size_t count = 0, j = 0;
        for (size_t i = 0; i < start.size(); ++i) {
            const auto edge = start[i];
            if (edge.rising != rising)
                continue;
            while (j < finish.size() && finish[j].time < edge.time)
                ++j;
            if (j < finish.size() && finish[j].rising == rising &&
                (i + 1 == start.size() || finish[j].time <= start[i + 1].time)) {
                sum += finish[j].time - edge.time;
                ++count;
            }
        }
        if (!count)
            return {};
        return sum / count;
    };
    m.rise_time = transitions(true);
    m.fall_time = transitions(false);
    return m;
}
} // namespace pds
