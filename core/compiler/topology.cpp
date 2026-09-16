#include "core/compiler/topology.hpp"
#include "core/model/semiconductor.hpp"
#include "core/model/waveform.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>
namespace pds {
namespace {
// Offset is V(node)-V(parent); union by size bounds tree depth.
struct Potentials {
    std::vector<int> parent, size;
    std::vector<long double> offset;
    explicit Potentials(int n) : parent(n), size(n, 1), offset(n) {
        std::iota(parent.begin(), parent.end(), 0);
    }
    std::pair<int, long double> find(int n) const {
        long double v = 0;
        while (parent[n] != n) {
            v += offset[n];
            n = parent[n];
        }
        return {n, v};
    }
    std::optional<long double> join(int p, int n, long double voltage) {
        auto [a, vp] = find(p);
        auto [b, vn] = find(n);
        if (a == b)
            return vp - vn - voltage;
        long double delta = voltage - vp + vn;
        if (size[a] > size[b]) {
            std::swap(a, b);
            delta = -delta;
        }
        parent[a] = b;
        offset[a] = delta;
        size[b] += size[a];
        return {};
    }
};
std::optional<Diagnostic> voltage_loop(const SimulationIR &ir, bool fixed_only, bool initialize,
                                       const std::vector<bool> &gates, const std::vector<bool> &diodes,
                                       const std::vector<double> &states, double time) {
    Potentials potentials(ir.node_count + 1);
    std::optional<Diagnostic> redundant;
    auto node = [&](int n) { return n < 0 ? ir.node_count : n; };
    for (size_t i = 0; i < ir.stamps.size(); ++i) {
        const auto &s = ir.stamps[i];
        const auto kind = s.component.kind;
        double voltage = 0;
        if (kind == Kind::voltage) {
            if (fixed_only && s.component.source.kind != Waveform::dc)
                continue;
            voltage = source_value(s.component, time, initialize ? TimeSide::right : TimeSide::left);
        } else if (kind == Kind::current_probe) {
        } else if (fixed_only)
            continue;
        else if (kind == Kind::capacitor && initialize)
            voltage = states[i];
        else if (kind == Kind::ideal_switch && gates[i] && !resistive_semiconductor(s.component)) {
        } else if (kind == Kind::diode && diodes[i] && !resistive_semiconductor(s.component)) {
        } else
            continue;
        if (auto mismatch = potentials.join(node(s.positive), node(s.negative), voltage)) {
            const long double tolerance =
                ir.profile.voltage_tolerance +
                ir.profile.relative_tolerance *
                    std::max(std::abs(static_cast<long double>(voltage)), std::abs(*mismatch + voltage));
            if (std::abs(*mismatch) > tolerance) {
                std::ostringstream message;
                message << "Incompatible ideal voltage constraints at '" << s.component.name
                        << "': loop mismatch " << static_cast<double>(*mismatch)
                        << " V. Check source polarity, closed switches and capacitor initial voltages.";
                return Diagnostic("conflicting_voltage_constraints", s.component.id, message.str(), time);
            }
            if (!redundant)
                redundant =
                    Diagnostic("ideal_voltage_loop", s.component.id,
                               "A loop of ideal voltage constraints has no unique branch currents. Check "
                               "voltage sources, "
                               "current probes, closed switches/diodes and initial capacitor constraints.",
                               time);
        }
    }
    return redundant;
}
} // namespace
void validate_source_loops(const SimulationIR &ir) {
    if (auto diagnostic = voltage_loop(ir, true, false, {}, {}, {}, 0))
        throw *diagnostic;
}
std::optional<Diagnostic> diagnose_singular_topology(const SimulationIR &ir, bool initialize,
                                                     const std::vector<bool> &gates,
                                                     const std::vector<bool> &diodes,
                                                     const std::vector<double> &states, double time) {
    if (auto loop = voltage_loop(ir, false, initialize, gates, diodes, states, time))
        return loop;
    Potentials islands(ir.node_count + 1);
    auto node = [&](int n) { return n < 0 ? ir.node_count : n; };
    for (size_t i = 0; i < ir.stamps.size(); ++i) {
        const auto &s = ir.stamps[i];
        const auto k = s.component.kind;
        const bool path = k == Kind::resistor || k == Kind::capacitor || k == Kind::voltage ||
                          k == Kind::current_probe || (k == Kind::inductor && !initialize) ||
                          (k == Kind::ideal_switch && (gates[i] || resistive_semiconductor(s.component))) ||
                          (k == Kind::diode && (diodes[i] || resistive_semiconductor(s.component)));
        if (path)
            islands.join(node(s.positive), node(s.negative), 0);
    }
    std::vector<long double> injection(ir.node_count + 1), magnitude(ir.node_count + 1);
    std::vector<std::string> source(ir.node_count + 1);
    for (size_t i = 0; i < ir.stamps.size(); ++i) {
        const auto &s = ir.stamps[i];
        double current;
        if (s.component.kind == Kind::current)
            current = source_value(s.component, time, initialize ? TimeSide::right : TimeSide::left);
        else if (s.component.kind == Kind::inductor && initialize)
            current = states[i];
        else
            continue;
        const int p = islands.find(node(s.positive)).first, n = islands.find(node(s.negative)).first;
        if (p == n)
            continue;
        injection[p] -= current;
        injection[n] += current;
        magnitude[p] += std::abs(current);
        magnitude[n] += std::abs(current);
        if (current != 0) {
            source[p] = s.component.id;
            source[n] = s.component.id;
        }
    }
    const int ground = islands.find(ir.node_count).first;
    std::optional<Diagnostic> floating;
    for (int i = 0; i < ir.node_count; ++i) {
        const int root = islands.find(i).first;
        if (root == ground)
            continue;
        if (std::abs(injection[root]) >
            ir.profile.current_tolerance + ir.profile.relative_tolerance * magnitude[root]) {
            std::ostringstream message;
            message << "A current cutset has no conducting return path; net injected current is "
                    << static_cast<double>(injection[root])
                    << " A. Check open switches/diodes and inductor initial currents.";
            return Diagnostic("current_cutset", source[root], message.str(), time);
        }
        if (!floating)
            floating =
                Diagnostic("floating_island", ir.unknowns[i].object,
                           "This island has no voltage-reference path in the current switch/diode state. "
                           "At initialization an inductor fixes current, not voltage.",
                           time);
    }
    return floating;
}
} // namespace pds
