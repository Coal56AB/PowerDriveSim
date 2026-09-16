#pragma once
#include "core/model/model.hpp"
namespace pds {
void validate_semiconductor(const Component &component);
double *semiconductor_parameter(Semiconductor &model, const std::string &key);
const double *semiconductor_parameter(const Semiconductor &model, const std::string &key);
bool semiconductor_property(Kind kind, const std::string &key);
inline bool resistive_semiconductor(const Component &c) {
    return c.semiconductor.model == SemiconductorModel::piecewise_linear;
}
inline bool dynamic_diode(const Component &c) {
    return c.kind == Kind::diode && resistive_semiconductor(c) && c.semiconductor.charge_dynamics;
}
struct DiodeChargeLaw {
    double alpha, lambda;
};
inline DiodeChargeLaw diode_charge_law(const Semiconductor &s) {
    return {(1 + s.carrier_lifetime / s.transit_time) * (1 / s.ron - 1 / s.roff),
            1 / s.transit_time + 1 / s.carrier_lifetime};
}
// Continuous diode I-V corner: I=V/Roff below Vf; above it dI/dV=1/Ron.
inline double diode_threshold(const Component &c) {
    return resistive_semiconductor(c) ? c.semiconductor.forward_voltage : 0;
}
inline double diode_threshold_current(const Component &c) {
    return resistive_semiconductor(c) ? c.semiconductor.forward_voltage / c.semiconductor.roff : 0;
}
} // namespace pds
