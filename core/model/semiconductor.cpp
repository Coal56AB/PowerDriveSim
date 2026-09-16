#include "core/model/semiconductor.hpp"
#include <cmath>
namespace pds {
namespace {
double Semiconductor::*parameter(const std::string &key) {
    if (key == "ron")
        return &Semiconductor::ron;
    if (key == "roff")
        return &Semiconductor::roff;
    if (key == "forward_voltage")
        return &Semiconductor::forward_voltage;
    return nullptr;
}
} // namespace
double *semiconductor_parameter(Semiconductor &s, const std::string &key) {
    const auto member = parameter(key);
    return member ? &(s.*member) : nullptr;
}
const double *semiconductor_parameter(const Semiconductor &s, const std::string &key) {
    const auto member = parameter(key);
    return member ? &(s.*member) : nullptr;
}
void validate_semiconductor(const Component &c) {
    const auto &s = c.semiconductor;
    if (unsigned(s.model) > unsigned(SemiconductorModel::piecewise_linear) || !std::isfinite(s.ron) ||
        !std::isfinite(s.roff) || !std::isfinite(s.forward_voltage) || s.ron <= 0 || s.roff <= s.ron ||
        s.forward_voltage < 0 ||
        ((c.kind != Kind::diode && c.kind != Kind::ideal_switch) && s != Semiconductor{}))
        throw Diagnostic("invalid_semiconductor", c.id,
                         "Semiconductor parameters require finite 0 < Ron < Roff and Vf >= 0; "
                         "the model applies only to switches and diodes");
}
} // namespace pds
