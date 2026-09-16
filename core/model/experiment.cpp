#include "core/model/model.hpp"
#include <algorithm>
#include <cmath>
#include <set>

namespace pds {
size_t experiment_size(const Experiment &e) {
    constexpr size_t maximum = 100000;
    auto text = [&](const std::string &s) {
        if (s.find_first_of("\r\n") != std::string::npos)
            throw Diagnostic("invalid_text", e.id, "Experiment text must be single-line");
    };
    auto target = [&](const ParameterTarget &t) {
        if (t.instances.size() > 64 || t.field.empty() || (!t.object.empty() && !valid_uuid(t.object)) ||
            (t.object.empty() && !t.instances.empty()))
            throw Diagnostic("invalid_sweep_target", t.object, "Invalid parameter target");
        text(t.field);
        for (const auto &id : t.instances)
            if (!valid_uuid(id))
                throw Diagnostic("invalid_sweep_target", id, "Invalid instance UUID");
    };
    text(e.name);
    if (!valid_uuid(e.id))
        throw Diagnostic("invalid_uuid", e.id, "Invalid experiment UUID");
    if (e.axes.size() > 64 || e.scenarios.size() > maximum || e.channels.size() > maximum)
        throw Diagnostic("sweep_size", e.id, "Too many experiment axes, scenarios or channels");
    if (!std::isfinite(e.begin) || !std::isfinite(e.end) || e.begin < 0 || (e.end != -1 && e.end <= e.begin))
        throw Diagnostic("invalid_sweep_range", e.id, "Invalid measurement interval");
    std::set<std::string> channels;
    for (const auto &channel : e.channels) {
        text(channel);
        if (channel.empty() || !channels.insert(channel).second)
            throw Diagnostic("invalid_recording", channel, "Empty or duplicate experiment channel");
    }
    size_t count = std::max<size_t>(1, e.scenarios.size()), budget = maximum;
    auto values = [&](size_t n) {
        if (n > budget)
            throw Diagnostic("sweep_size", e.id, "Too many experiment parameter values");
        budget -= n;
    };
    auto finite = [&](double value) {
        if (!std::isfinite(value))
            throw Diagnostic("invalid_sweep_value", e.id, "Sweep values must be finite");
    };
    for (const auto &axis : e.axes) {
        target(axis.target);
        values(axis.values.size());
        if (axis.values.empty() || axis.values.size() > maximum / count)
            throw Diagnostic("sweep_size", e.id, "Sweep is empty or exceeds 100000 cases");
        count *= axis.values.size();
        for (double value : axis.values)
            finite(value);
    }
    for (const auto &scenario : e.scenarios) {
        text(scenario.name);
        values(scenario.overrides.size());
        for (const auto &v : scenario.overrides) {
            target(v.target);
            finite(v.value);
        }
    }
    return count;
}
} // namespace pds
