#include "core/model/waveform.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
namespace pds {
namespace {
double SourceWaveform::*parameter(const std::string &key) {
    if (key == "source_offset")
        return &SourceWaveform::offset;
    if (key == "source_frequency")
        return &SourceWaveform::frequency;
    if (key == "source_phase")
        return &SourceWaveform::phase;
    if (key == "source_delay")
        return &SourceWaveform::delay;
    if (key == "source_duty")
        return &SourceWaveform::duty;
    return nullptr;
}
double pulse_edge(const SourceWaveform &s, double cycle, bool falling) {
    return s.delay + (cycle + (falling ? s.duty : 0)) / s.frequency;
}
} // namespace
double *source_parameter(SourceWaveform &s, const std::string &key) {
    auto member = parameter(key);
    return member ? &(s.*member) : nullptr;
}
const double *source_parameter(const SourceWaveform &s, const std::string &key) {
    auto member = parameter(key);
    return member ? &(s.*member) : nullptr;
}
void validate_waveform(const Component &c) {
    const auto &s = c.source;
    auto invalid = [&] {
        throw Diagnostic("invalid_waveform", c.id,
                         "Source waveform requires finite parameters, positive frequency, duty in [0,1], "
                         "nonnegative delay and strictly increasing nonnegative table times");
    };
    if (unsigned(s.kind) > unsigned(Waveform::piecewise_linear) || !std::isfinite(s.offset) ||
        !std::isfinite(s.frequency) || s.frequency <= 0 || !std::isfinite(s.phase) ||
        !std::isfinite(s.delay) || s.delay < 0 || !std::isfinite(s.duty) || s.duty < 0 || s.duty > 1)
        invalid();
    if (c.kind != Kind::voltage && c.kind != Kind::current) {
        if (s != SourceWaveform{})
            invalid();
        return;
    }
    if (s.kind == Waveform::piecewise_linear && s.points.empty())
        invalid();
    for (size_t i = 0; i < s.points.size(); ++i)
        if (!std::isfinite(s.points[i].x) || s.points[i].x < 0 || !std::isfinite(s.points[i].y) ||
            (i && s.points[i].x <= s.points[i - 1].x))
            invalid();
}
double source_value(const Component &c, double time, TimeSide side) {
    const auto &s = c.source;
    if (s.kind == Waveform::dc)
        return c.value;
    if (s.kind == Waveform::piecewise_linear) {
        auto end = std::upper_bound(s.points.begin(), s.points.end(), time,
                                    [](double t, const Point &p) { return t < p.x; });
        if (end == s.points.begin())
            return end->y;
        if (end == s.points.end())
            return s.points.back().y;
        const auto &a = *(end - 1), &b = *end;
        const auto fraction = (time - a.x) / (b.x - a.x);
        return a.y * (1 - fraction) + b.y * fraction;
    }
    if (time < s.delay || (time == s.delay && side == TimeSide::left))
        return s.offset;
    if (s.kind == Waveform::sine)
        return s.offset + c.value * std::sin(2 * std::numbers::pi * s.frequency * (time - s.delay) + s.phase);
    if (s.duty == 0)
        return s.offset;
    if (s.duty == 1)
        return s.offset + c.value;
    const double cycle = std::floor((time - s.delay) * s.frequency);
    // Compare the timestamps constructed by the scheduler; no epsilon shifts.
    for (int offset = -1; offset <= 1; ++offset) {
        const double k = cycle + offset;
        if (k < 0)
            continue;
        if (time == pulse_edge(s, k, false))
            return s.offset + (side == TimeSide::right ? c.value : 0);
        if (time == pulse_edge(s, k, true))
            return s.offset + (side == TimeSide::left ? c.value : 0);
    }
    return s.offset + (((time - s.delay) * s.frequency - cycle) < s.duty ? c.value : 0);
}
double next_source_breakpoint(const Component &c, double time) {
    const auto &s = c.source;
    double next = std::numeric_limits<double>::infinity();
    if (s.kind == Waveform::dc)
        return next;
    if (s.kind == Waveform::piecewise_linear) {
        auto point = std::upper_bound(s.points.begin(), s.points.end(), time,
                                      [](double t, const Point &p) { return t < p.x; });
        return point == s.points.end() ? next : point->x;
    }
    if (s.kind == Waveform::pulse && s.duty == 0)
        return next;
    if (time < s.delay)
        return s.delay;
    if (s.kind == Waveform::sine || s.duty == 1)
        return next;
    const double cycle = std::floor((time - s.delay) * s.frequency);
    if (!std::isfinite(cycle) || cycle + 1 == cycle)
        throw Diagnostic("time_resolution", c.id, "Source period cannot advance floating-point time", time);
    for (int offset = -1; offset <= 2; ++offset) {
        const double k = cycle + offset;
        if (k < 0)
            continue;
        for (bool falling : {false, true}) {
            const auto edge = pulse_edge(s, k, falling);
            if (edge > time)
                next = std::min(next, edge);
        }
    }
    return next;
}
} // namespace pds
