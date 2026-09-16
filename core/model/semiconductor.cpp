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
    if (key == "transit_time")
        return &Semiconductor::transit_time;
    if (key == "carrier_lifetime")
        return &Semiconductor::carrier_lifetime;
    if (key == "initial_charge")
        return &Semiconductor::initial_charge;
    if (key == "holding_current")
        return &Semiconductor::holding_current;
    return nullptr;
}
} // namespace
bool semiconductor_property(Kind kind, const std::string &key) {
    if (key == "ron" || key == "roff")
        return gate_controlled(kind) || kind == Kind::diode;
    if (key == "forward_voltage")
        return rectifying(kind);
    if (key == "holding_current")
        return kind == Kind::thyristor;
    return kind == Kind::diode &&
           (key == "transit_time" || key == "carrier_lifetime" || key == "initial_charge");
}
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
        ((!rectifying(c.kind) && c.kind != Kind::ideal_switch) && s != Semiconductor{}))
        throw Diagnostic("invalid_semiconductor", c.id,
                         "Semiconductor parameters require finite 0 < Ron < Roff and Vf >= 0; "
                         "the model applies only to switches, diodes and thyristors");
    if (!std::isfinite(s.holding_current) || s.holding_current < 0 ||
        (c.kind != Kind::thyristor && (s.holding_current != 0 || s.initial_latched)))
        throw Diagnostic(
            "invalid_thyristor", c.id,
            "Holding current must be finite and nonnegative; latch parameters require a thyristor");
    const auto law = diode_charge_law(s);
    if (!std::isfinite(s.transit_time) || s.transit_time <= 0 || !std::isfinite(s.carrier_lifetime) ||
        s.carrier_lifetime <= 0 || !std::isfinite(s.initial_charge) || s.initial_charge < 0 ||
        !std::isfinite(law.alpha) || !std::isfinite(law.lambda) ||
        (c.kind != Kind::diode &&
         (s.charge_dynamics || s.transit_time != Semiconductor{}.transit_time ||
          s.carrier_lifetime != Semiconductor{}.carrier_lifetime || s.initial_charge != 0)))
        throw Diagnostic("invalid_diode_charge", c.id,
                         "Diode charge requires finite positive transit time and carrier lifetime, "
                         "nonnegative initial charge and representable coefficients");
}
} // namespace pds
