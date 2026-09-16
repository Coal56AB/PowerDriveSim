#pragma once
#include "core/model/model.hpp"
namespace pds {
void validate_semiconductor(const Component &component);
double *semiconductor_parameter(Semiconductor &model, const std::string &key);
const double *semiconductor_parameter(const Semiconductor &model, const std::string &key);
inline bool resistive_semiconductor(const Component &c) {
    return c.semiconductor.model == SemiconductorModel::piecewise_linear;
}
// Continuous diode I-V corner: I=V/Roff below Vf; above it dI/dV=1/Ron.
inline double diode_threshold(const Component &c) {
    return resistive_semiconductor(c) ? c.semiconductor.forward_voltage : 0;
}
inline double diode_threshold_current(const Component &c) {
    return resistive_semiconductor(c) ? c.semiconductor.forward_voltage / c.semiconductor.roff : 0;
}
} // namespace pds
