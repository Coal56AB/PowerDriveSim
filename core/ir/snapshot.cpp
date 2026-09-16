#include "core/ir/snapshot.hpp"
#include "core/model/semiconductor.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
namespace pds {
std::string snapshot_contract(const SimulationIR &ir, double time) {
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << std::setprecision(std::numeric_limits<double>::max_digits10);
    text << "pds-state-v1 " << ir.project_id << ' ' << ir.profile.step << ' '
         << static_cast<int>(ir.profile.method) << ' ' << ir.profile.max_iterations << ' '
         << ir.profile.voltage_tolerance << ' ' << ir.profile.current_tolerance << ' '
         << ir.profile.relative_tolerance << ' ' << ir.node_count << '\n';
    if (ir.profile.step_control.adaptive) {
        const auto &control = ir.profile.step_control;
        text << "adaptive " << control.minimum_step << ' ' << control.relative_tolerance << ' '
             << control.voltage_tolerance << ' ' << control.current_tolerance << ' ' << control.charge_tolerance << '\n';
    }
    for (const auto &channel : ir.unknowns)
        text << channel.object << '\n';
    for (const auto &stamp : ir.stamps) {
        const auto &c = stamp.component;
        const auto &s = c.source;
        const auto &d = c.semiconductor;
        text << c.id << ' ' << static_cast<int>(c.kind) << ' ' << stamp.positive << ' ' << stamp.negative
             << ' ' << stamp.branch << ' ' << c.value << ' ' << c.initial << ' ' << c.closed << ' '
             << static_cast<int>(s.kind) << ' ' << s.offset << ' ' << s.frequency << ' ' << s.phase << ' '
             << s.delay << ' ' << s.duty << ' ' << s.points.size();
        for (const auto &point : s.points)
            text << ' ' << point.x << ' ' << point.y;
        text << ' ' << static_cast<int>(d.model) << ' ' << d.ron << ' ' << d.roff << ' ' << d.forward_voltage
             << ' ' << d.charge_dynamics << ' ' << d.transit_time << ' ' << d.carrier_lifetime << ' '
             << d.initial_charge << ' ' << d.holding_current << ' ' << d.initial_latched << '\n';
    }
    for (const auto &signal : ir.gate_signals)
        text << signal.id << ' ' << signal.initial << '\n';
    for (const auto &event : ir.events) {
        if (event.time > time)
            break;
        text << event.target << ' ' << event.time << ' ' << event.closed << '\n';
    }
    return derived_uuid(text.str());
}
void validate_snapshot(const SimulationSnapshot &s, const SimulationIR &ir) {
    const auto invalid = [&](const char *message) {
        throw Diagnostic("invalid_snapshot", ir.project_id, message);
    };
    if ((s.version != 1 && s.version != 2) || !std::isfinite(s.time) || s.time < 0 || s.time > ir.profile.stop ||
        s.next_grid == 0)
        invalid("Snapshot version or time is invalid for this run");
    if (s.project_id != ir.project_id || s.contract != snapshot_contract(ir, s.time))
        invalid("Snapshot belongs to a different model, integration profile or past gate schedule");
    if (!std::isfinite(s.next_step) || (ir.profile.step_control.adaptive
        ? s.version < 2 || s.next_step < ir.profile.step_control.minimum_step || s.next_step > ir.profile.step
        : s.next_step != 0))
        invalid("Snapshot adaptive step is invalid");
    const auto count = ir.stamps.size();
    if (s.states.size() != count || s.history.size() != count || s.gates.size() != count ||
        s.diodes.size() != count || s.latched.size() != count || s.values.size() != ir.unknowns.size() ||
        s.signal_values.size() != ir.gate_signals.size())
        invalid("Snapshot arrays do not match the model");
    for (const auto *values : {&s.states, &s.history, &s.values})
        if (std::any_of(values->begin(), values->end(), [](double v) { return !std::isfinite(v); }))
            invalid("Snapshot contains a nonfinite value");
    const double next = static_cast<double>(s.next_grid) * ir.profile.step;
    const double previous = static_cast<double>(s.next_grid - 1) * ir.profile.step;
    if (!std::isfinite(next) || next <= s.time || previous > s.time)
        invalid("Snapshot grid position does not match its time");
    for (size_t k = 0; k < count; ++k) {
        const auto &stamp = ir.stamps[k];
        const auto voltage = (stamp.positive < 0 ? 0 : s.values.at(stamp.positive)) -
                             (stamp.negative < 0 ? 0 : s.values.at(stamp.negative));
        if (stamp.component.kind == Kind::capacitor &&
            (s.states[k] != voltage || s.history[k] != s.values.at(stamp.branch)))
            invalid("Snapshot capacitor state and algebraic values disagree");
        if (stamp.component.kind == Kind::inductor &&
            (s.states[k] != s.values.at(stamp.branch) || s.history[k] != voltage))
            invalid("Snapshot inductor state and algebraic values disagree");
        if (dynamic_diode(stamp.component) && s.states[k] < 0)
            invalid("Snapshot diode charge is negative");
    }
}
} // namespace pds
