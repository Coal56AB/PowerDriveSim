#pragma once
#include "core/ir/ir.hpp"
#include "core/model/semiconductor.hpp"
#include <algorithm>
#include <cmath>

namespace pds {
struct StepError {
    double ratio = 0;
    std::string object;
};
// Step doubling estimates the fine solution's local truncation error. Algebraic
// unknowns and stored diode charge use separate physical absolute tolerances.
inline StepError step_error(const SimulationIR &ir, const std::vector<double> &before,
                            const std::vector<double> &coarse, const std::vector<double> &fine,
                            const std::vector<double> &coarse_states,
                            const std::vector<double> &fine_states) {
    StepError error;
    const auto &control = ir.profile.step_control;
    const double divisor = ir.profile.method == Method::trapezoidal ? 3 : 1;
    auto include = [&](double a, double b, double scale, double absolute, const std::string &id) {
        const double tolerance = absolute + control.relative_tolerance * scale;
        const double ratio = std::abs(a - b) / (divisor * tolerance);
        if (ratio > error.ratio)
            error = {ratio, id};
    };
    for (size_t k = 0; k < fine.size(); ++k)
        include(coarse[k], fine[k], std::max(std::abs(before[k]), std::abs(fine[k])),
                k < static_cast<size_t>(ir.node_count) ? control.voltage_tolerance
                                                       : control.current_tolerance,
                ir.unknowns[k].object);
    for (size_t k = 0; k < ir.stamps.size(); ++k)
        if (dynamic_diode(ir.stamps[k].component))
            include(coarse_states[k], fine_states[k],
                    std::max(std::abs(coarse_states[k]), std::abs(fine_states[k])), control.charge_tolerance,
                    ir.stamps[k].component.id);
    return error;
}
inline double next_step_factor(Method method, double error) {
    if (error == 0)
        return 2;
    const double order = method == Method::trapezoidal ? 3 : 2;
    return std::clamp(.9 * std::pow(error, -1 / order), .2, 2.0);
}
} // namespace pds
