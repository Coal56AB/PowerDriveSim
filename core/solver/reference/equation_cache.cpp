#include "core/solver/reference/equation_cache.hpp"
#include "core/model/semiconductor.hpp"
#include "core/model/waveform.hpp"
namespace pds {
const std::vector<double> &EquationCache::solve(double time, double h, bool initialize,
                                                const std::vector<bool> &gates,
                                                const std::vector<bool> &diodes,
                                                const std::vector<double> &states,
                                                const std::vector<double> &history) {
    const bool trapezoidal = ir_.profile.method == Method::trapezoidal;
    auto matches = [&](size_t i) {
        const auto &e = *entries_[i];
        return e.h == h && e.initialize == initialize && e.gates == gates && e.diodes == diodes;
    };
    size_t found = entries_.size();
    if (recent_ < entries_.size() && matches(recent_))
        found = recent_;
    else
        for (size_t i = 0; i < entries_.size(); ++i)
            if (matches(i)) {
                found = i;
                break;
            }
    if (found == entries_.size()) {
        auto entry = std::make_unique<Entry>(ir_.unknowns.size());
        entry->h = h;
        entry->initialize = initialize;
        entry->gates = gates;
        entry->diodes = diodes;
        auto &system = entry->system;
        for (size_t i = 0; i < ir_.stamps.size(); ++i) {
            const auto &s = ir_.stamps[i];
            const auto &c = s.component;
            const int p = s.positive, n = s.negative, b = s.branch;
            if (b >= 0)
                system.incidence(p, n, b);
            switch (c.kind) {
            case Kind::resistor:
                system.conductance(p, n, 1 / c.value);
                break;
            case Kind::current:
                if (c.source.kind == Waveform::dc) {
                    system.inject(p, -c.value);
                    system.inject(n, c.value);
                } else {
                    entry->dynamic.push_back({i, p, c.kind, -1});
                    entry->dynamic.push_back({i, n, c.kind, 1});
                }
                break;
            case Kind::voltage:
                system.add(b, p, 1);
                system.add(b, n, -1);
                if (c.source.kind == Waveform::dc)
                    system.inject(b, c.value);
                else
                    entry->dynamic.push_back({i, b, c.kind, 1});
                break;
            case Kind::capacitor:
                system.add(b, p, 1);
                system.add(b, n, -1);
                if (!initialize)
                    system.add(b, b, -h / (c.value * (trapezoidal ? 2 : 1)));
                entry->dynamic.push_back({i, b, c.kind, !initialize && trapezoidal ? h / (2 * c.value) : 0});
                break;
            case Kind::inductor: {
                const double factor = initialize ? 1 : c.value / h * (trapezoidal ? 2 : 1);
                if (initialize)
                    system.add(b, b, 1);
                else {
                    system.add(b, p, 1);
                    system.add(b, n, -1);
                    system.add(b, b, -factor);
                }
                entry->dynamic.push_back({i, b, c.kind, factor});
                break;
            }
            case Kind::voltage_probe:
                break;
            case Kind::current_probe:
                system.add(b, p, 1);
                system.add(b, n, -1);
                break;
            case Kind::igbt:
            case Kind::thyristor:
            case Kind::diode:
            case Kind::ideal_switch:
                if (dynamic_diode(c)) {
                    const auto &model = c.semiconductor;
                    const auto law = diode_charge_law(model);
                    const double m = initialize ? 0 : h / (trapezoidal ? 2 : 1);
                    const double k = 1 / (1 + m * law.lambda);
                    const double forward = diodes[i] ? law.alpha * k * (1 + m / model.carrier_lifetime) : 0;
                    const double conductance = 1 / model.roff + forward;
                    system.add(b, p, -conductance);
                    system.add(b, n, conductance);
                    system.add(b, b, 1);
                    system.inject(b, -forward * model.forward_voltage);
                    entry->dynamic.push_back({i, b, c.kind, -k / model.transit_time});
                    break;
                }
                if (resistive_semiconductor(c)) {
                    const auto &model = c.semiconductor;
                    const bool active = rectifying(c.kind) ? diodes[i] : gates[i];
                    if (active) {
                        system.add(b, p, 1);
                        system.add(b, n, -1);
                        system.add(b, b, -model.ron);
                        if (rectifying(c.kind))
                            system.inject(b, model.forward_voltage * (1 - model.ron / model.roff));
                    } else {
                        system.add(b, p, -1 / model.roff);
                        system.add(b, n, 1 / model.roff);
                        system.add(b, b, 1);
                    }
                    break;
                }
                if (rectifying(c.kind) ? diodes[i] : gates[i]) {
                    system.add(b, p, 1);
                    system.add(b, n, -1);
                } else
                    system.add(b, b, 1);
                break;
            }
        }
        entry->constant = system.rhs;
        // Bound storage for large sparse circuits as well as small dense ones.
        const size_t limit = ir_.unknowns.size() <= 64 ? 32 : 2;
        if (entries_.size() < limit)
            entries_.push_back(std::move(entry));
        else {
            found = replace_;
            replace_ = (replace_ + 1) % limit;
            entries_[found] = std::move(entry);
        }
    }
    recent_ = found;
    auto &entry = *entries_[found];
    entry.system.rhs = entry.constant;
    for (const auto &term : entry.dynamic) {
        double value;
        if (term.kind == Kind::voltage || term.kind == Kind::current)
            value = term.factor * source_value(ir_.stamps[term.state].component, time,
                                               initialize ? TimeSide::right : TimeSide::left);
        else if (term.kind == Kind::diode)
            value = term.factor *
                    (states[term.state] + (!initialize && trapezoidal ? h / 2 * history[term.state] : 0));
        else if (term.kind == Kind::capacitor)
            value = states[term.state] + (!initialize && trapezoidal ? term.factor * history[term.state] : 0);
        else
            value = initialize ? states[term.state]
                               : -term.factor * states[term.state] - (trapezoidal ? history[term.state] : 0);
        entry.system.inject(term.row, value);
    }
    return entry.factorization.solve_fixed(entry.system, ir_, time);
}
} // namespace pds
