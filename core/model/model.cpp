#include "core/model/model.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
namespace pds {
void validate_motor(const Component &component) {
    if (component.kind != Kind::dc_motor)
        return;
    const auto &motor = component.motor;
    if (!std::isfinite(component.value) || component.value <= 0 ||
        !std::isfinite(motor.inertia) || motor.inertia <= 0 ||
        !std::isfinite(motor.damping) || motor.damping < 0 ||
        !std::isfinite(motor.torque_constant) || motor.torque_constant <= 0 ||
        !std::isfinite(motor.back_emf_constant) || motor.back_emf_constant <= 0 ||
        !std::isfinite(motor.load_torque) || !std::isfinite(component.initial))
        throw Diagnostic("invalid_parameter", component.id,
                         "DC motor requires finite R, J, Kt and Ke > 0, B >= 0, and finite load torque and initial speed");
    const double scale = std::max({1.0, std::abs(motor.torque_constant), std::abs(motor.back_emf_constant)});
    if (std::abs(motor.torque_constant - motor.back_emf_constant) > 1e-12 * scale)
        throw Diagnostic("invalid_parameter", component.id,
                         "A lossless SI permanent-magnet coupling requires equal torque and back-EMF constants Kt = Ke");
}
void validate_step_control(const Profile &profile, const std::string &object) {
    const auto &control = profile.step_control;
    for (double value : {control.minimum_step, control.voltage_tolerance,
                         control.current_tolerance, control.charge_tolerance})
        if (!std::isfinite(value) || value <= 0)
            throw Diagnostic("invalid_profile", object, "Adaptive step limits and tolerances must be finite and positive");
    if (!std::isfinite(control.relative_tolerance) || control.relative_tolerance < 0)
        throw Diagnostic("invalid_profile", object, "Adaptive relative tolerance must be finite and nonnegative");
    if (control.adaptive && control.minimum_step > profile.step)
        throw Diagnostic("invalid_profile", object, "Minimum step exceeds the maximum step");
}
std::string initial_state_name(InitialState state) {
    switch (state) {
    case InitialState::specified: return "Specified";
    case InitialState::zero: return "Zero";
    case InitialState::dc_operating_point: return "DCOperatingPoint";
    }
    throw Diagnostic("invalid_initial_state", "", "Unsupported initial state mode");
}
InitialState parse_initial_state(const std::string &name) {
    for (auto state : {InitialState::specified, InitialState::zero, InitialState::dc_operating_point})
        if (initial_state_name(state) == name) return state;
    throw Diagnostic("invalid_initial_state", name, "Unsupported initial state mode");
}
std::string method_name(Method method) {
    switch(method) {
    case Method::backward_euler: return "BackwardEuler";
    case Method::trapezoidal: return "Trapezoidal";
    }
    throw Diagnostic("invalid_method","","Unsupported integration method");
}
Method parse_method(const std::string& name) {
    for(auto m:{Method::backward_euler,Method::trapezoidal}) if(method_name(m)==name) return m;
    throw Diagnostic("invalid_method",name,"Unsupported integration method");
}
std::string kind_name(Kind k) {
    switch(k) {
    case Kind::resistor: return "R";
    case Kind::capacitor: return "C";
    case Kind::inductor: return "L";
    case Kind::voltage: return "V";
    case Kind::current: return "I";
    case Kind::ideal_switch: return "S";
    case Kind::diode: return "D";
    case Kind::thyristor: return "T";
    case Kind::igbt: return "IGBT";
    case Kind::voltage_probe: return "VP";
    case Kind::current_probe: return "IP";
    case Kind::ideal_transformer: return "Transformer";
    case Kind::dc_motor: return "DCM";
    }
    throw Diagnostic("unknown_component", "", "Unsupported component kind");
}
Kind parse_kind(const std::string& s) {
    for(auto k : {Kind::resistor, Kind::capacitor, Kind::inductor, Kind::voltage, Kind::current, Kind::ideal_switch, Kind::diode, Kind::voltage_probe, Kind::current_probe, Kind::thyristor, Kind::igbt, Kind::ideal_transformer, Kind::dc_motor})
        if(kind_name(k) == s) return k;
    throw Diagnostic("unknown_component", s, "Unsupported component type");
}
Diagnostic::Diagnostic(std::string c, std::string o, std::string m, double t)
    : std::runtime_error(std::move(m)), code(std::move(c)), object(std::move(o)), time(t) {}
bool valid_uuid(const std::string& s) {
    if(s.size()!=36) return false;
    for(size_t i=0;i<s.size();++i) {
        if(i==8 || i==13 || i==18 || i==23) { if(s[i]!='-') return false; }
        else if(!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
        else if(s[i]>='A' && s[i]<='F') return false;
    }
    return true;
}
}
