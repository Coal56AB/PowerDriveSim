#include "core/editor/properties.hpp"
#include "core/model/hierarchy.hpp"
#include "core/model/waveform.hpp"
#include "core/model/semiconductor.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <numbers>
#include <optional>
namespace pds {
namespace {
constexpr const char *kThreePhaseYDefinition = "1a963f2c-ceb8-5cce-b927-44d735ec9e80";
constexpr const char *kThreePhaseDeltaDefinition = "eb613164-faf4-5b03-9014-806885fef344";
constexpr const char *kThreePhaseVoltageParameter = "6c1aaf47-7a4f-5dac-ab25-cdd71f6816b3";
constexpr const char *kThreePhaseVoltageKindParameter = "9e07a7ea-8295-5fd0-92c6-6e82c8f1c21b";
constexpr const char *kThreePhaseSwitchRon = "f0d30ae4-bd4b-5ad7-84b1-467ff17c1000";
constexpr const char *kThreePhaseSwitchRoff = "f34a470a-c84b-5d03-bbe3-437926541000";

bool three_phase_source_definition(const std::string &id) {
    return id == kThreePhaseYDefinition || id == kThreePhaseDeltaDefinition;
}
std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return value;
}
const Definition *find_definition(const Project &p, const std::string &id) {
    auto found = std::find_if(p.definitions.begin(), p.definitions.end(),
                              [&](const Definition &definition) { return definition.id == id; });
    return found == p.definitions.end() ? nullptr : &*found;
}
bool three_phase_source_definition(const Project &p, const std::string &id) {
    if (three_phase_source_definition(id))
        return true;
    const auto *d = find_definition(p, id);
    if (!d)
        return false;
    if (lower_ascii(d->name).find("three-phase voltage source") != std::string::npos)
        return true;
    bool has_voltage = false, has_frequency = false;
    for (const auto &parameter : d->parameters) {
        has_voltage |= parameter.id == kThreePhaseVoltageParameter || parameter.field == "value";
        has_frequency |= parameter.field == "source_frequency";
    }
    return has_voltage && has_frequency && d->ports.size() >= 4;
}
bool three_phase_delta_definition(const Project &p, const std::string &id) {
    if (id == kThreePhaseDeltaDefinition)
        return true;
    const auto *d = find_definition(p, id);
    if (!d)
        return false;
    const auto name = lower_ascii(d->name);
    return name.find("delta") != std::string::npos || name.find("triangle") != std::string::npos;
}
bool synthetic_instance_parameter(const std::string &parameter) {
    return parameter == kThreePhaseSwitchRon || parameter == kThreePhaseSwitchRoff;
}
double instance_parameter_value(const Project &p, const Instance &i, const std::string &parameter) {
    for (const auto &[key, value] : i.parameters)
        if (key == parameter)
            return value;
    for (const auto &param : definition(p, i.definition).parameters)
        if (param.id == parameter)
            return param.value;
    throw Diagnostic("unknown_property", i.id, "Unsupported property: parameter/" + parameter);
}
double instance_parameter_value_or(const Project &p, const Instance &i, const std::string &parameter, double fallback) {
    try {
        return instance_parameter_value(p, i, parameter);
    } catch (const Diagnostic &) {
        return fallback;
    }
}
void set_instance_parameter(Instance &i, const std::string &parameter, double value) {
    auto found = std::find_if(i.parameters.begin(), i.parameters.end(),
                              [&](const auto &entry) { return entry.first == parameter; });
    if (found == i.parameters.end())
        i.parameters.emplace_back(parameter, value);
    else
        found->second = value;
}
double three_phase_display_voltage(const Project &p, const Instance &i) {
    const bool delta = three_phase_delta_definition(p, i.definition);
    const auto kind = unsigned(instance_parameter_value_or(p, i, kThreePhaseVoltageKindParameter, 2.0));
    const double internal = instance_parameter_value_or(p, i, kThreePhaseVoltageParameter, 310.0);
    const double phase_peak = delta ? internal / std::sqrt(3.0) : internal;
    const double line_peak = delta ? internal : internal * std::sqrt(3.0);
    switch (kind) {
    case 0:
        return phase_peak / std::sqrt(2.0);
    case 1:
        return line_peak / std::sqrt(2.0);
    case 2:
        return phase_peak;
    case 3:
        return line_peak;
    default:
        return phase_peak / std::sqrt(2.0);
    }
}
double three_phase_internal_voltage(unsigned kind, bool delta, double display) {
    double phase_peak = display;
    switch (kind) {
    case 0:
        phase_peak = display * std::sqrt(2.0);
        break;
    case 1:
        phase_peak = display * std::sqrt(2.0) / std::sqrt(3.0);
        break;
    case 2:
        phase_peak = display;
        break;
    case 3:
        phase_peak = display / std::sqrt(3.0);
        break;
    default:
        break;
    }
    return delta ? phase_peak * std::sqrt(3.0) : phase_peak;
}
} // namespace

bool external_gate(const Project &p, const std::string &id) {
    return std::any_of(p.wires.begin(), p.wires.end(), [&](const Wire &w) {
        return w.from == Endpoint{id, "gate"} || w.to == Endpoint{id, "gate"};
    });
}
std::string object_type(const Project &p, const std::string &id) {
    for (const auto &c : p.components)
        if (c.id == id)
            return kind_name(c.kind);
    for (const auto &n : p.nodes)
        if (n.id == id)
            return n.ground ? "ground" : "node";
    for (const auto &t : p.tags)
        if (t.id == id)
            return "tag";
    for (const auto &g : p.patterns)
        if (g.id == id)
            return "pattern";
    for (const auto &g : p.plots)
        if (g.id == id)
            return g.differential ? "differential_plot" : "plot";
    for(const auto& i:p.instances)if(i.id==id)return "instance:"+i.definition;
    for (const auto &w : p.wires)
        if (w.id == id)
            return "wire";
    throw Diagnostic("missing_object", id, "Object no longer exists");
}
PropertyValue read_property(const Project &p, const std::string &id, const std::string &key) {
    if (key == "events") {
        std::vector<GateEvent> result;
        for (auto e : p.events)
            if (e.target == id)
                result.push_back(e);
        return result;
    }
    auto common = [&](const auto &o) -> std::optional<PropertyValue> {
        if (key == "name")
            return o.name;
        if (key == "x")
            return o.x;
        if (key == "y")
            return o.y;
        return {};
    };
    for(const auto& i:p.instances)if(i.id==id) {
        if(auto v=common(i))return *v;
        if (three_phase_source_definition(p, i.definition)) {
            if (key == "three_phase_connection")
                return unsigned(three_phase_delta_definition(p, i.definition) ? 1 : 0);
            if (key == "three_phase_voltage_kind")
                return unsigned(instance_parameter_value_or(p, i, kThreePhaseVoltageKindParameter, 2.0));
            if (key == "three_phase_voltage")
                return three_phase_display_voltage(p, i);
        }
        if(key.rfind("parameter/",0)==0) {
            const auto parameter=key.substr(10);
            if (three_phase_source_definition(p, i.definition)) {
                if (parameter == kThreePhaseVoltageKindParameter)
                    return unsigned(instance_parameter_value_or(p, i, kThreePhaseVoltageKindParameter, 2.0));
                if (parameter == kThreePhaseVoltageParameter)
                    return three_phase_display_voltage(p, i);
            }
            for(const auto& param:definition(p,i.definition).parameters)if(param.id==parameter) {
                for(const auto& override:i.parameters)if(override.first==parameter)return override.second;
                return param.value;
            }
            if (synthetic_instance_parameter(parameter))
                throw Diagnostic("unknown_property", id, "Unsupported property: " + key);
        }
    }
    for (const auto &c : p.components)
        if (c.id == id) {
            if (auto v = common(c))
                return *v;
            if (key == "value")
                return c.value;
            if (key == "initial")
                return c.initial;
            if (key == "parallel_resistance_enabled")
                return c.parallel_resistance_enabled;
            if (key == "parallel_resistance")
                return c.parallel_resistance;
            if (key == "closed")
                return c.closed;
            if(rectifying(c.kind)||gate_controlled(c.kind)) {
                if(key=="semiconductor_model")return unsigned(c.semiconductor.model);
                if(c.kind==Kind::diode&&key=="charge_model")return unsigned(c.semiconductor.charge_dynamics);
                if(c.kind==Kind::thyristor&&key=="initial_latched")return c.semiconductor.initial_latched;
                if(semiconductor_property(c.kind,key))
                    if(auto v=semiconductor_parameter(c.semiconductor,key))return *v;
            }
            if(c.kind==Kind::voltage||c.kind==Kind::current) {
                if(key=="source_mode")return unsigned(c.source.kind);
                if(key=="source_points")return c.source.points;
                if(key=="source_phase_deg")return c.source.phase*180.0/std::numbers::pi;
                if(auto v=source_parameter(c.source,key))return *v;
            }
        }
    for (const auto &n : p.nodes)
        if (n.id == id)
            if (auto v = common(n))
                return *v;
    for (const auto &t : p.tags)
        if (t.id == id) {
            if (auto v = common(t))
                return *v;
            if (key == "tag_domain")
                return unsigned(t.domain);
            if (key == "tag_scope")
                return unsigned(t.scope);
            if (key == "tag_listed")
                return t.listed;
        }
    for (const auto &g : p.patterns)
        if (g.id == id) {
            if (auto v = common(g))
                return *v;
            if (key == "gate_mode")
                return unsigned(g.script ? 2 : g.pwm ? 1 : 0);
            if (key == "closed")
                return g.initial;
            if (key == "frequency")
                return g.frequency;
            if (key == "duty")
                return g.duty;
            if (key == "delay")
                return g.delay;
            if (key == "gate_code")
                return g.code;
            if (key == "script_step")
                return g.script_step;
        }
    for (const auto &g : p.plots)
        if (g.id == id) {
            if (auto v = common(g))
                return *v;
            if (key == "inputs")
                return g.inputs;
        }
    for (const auto &w : p.wires)
        if (w.id == id) {
            if (key == "wire_color")
                return w.color;
            if (key == "wire_width")
                return w.width;
            if (key == "wire_line")
                return unsigned(w.line);
        }
    throw Diagnostic("unknown_property", id, "Unsupported property: " + key);
}
void write_property(Project &p, const std::string &id, const std::string &key, const PropertyValue &value) {
    (void)read_property(p, id, key); // Validate the binding before mutation.
    for(auto& i:p.instances)if(i.id==id) {
        if(key=="name"){i.name=std::get<std::string>(value);return;}
        if(key=="x"){i.x=std::get<double>(value);return;}
        if(key=="y"){i.y=std::get<double>(value);return;}
        if (three_phase_source_definition(p, i.definition)) {
            if (key == "three_phase_connection") {
                const double display = three_phase_display_voltage(p, i);
                const auto kind = unsigned(instance_parameter_value_or(p, i, kThreePhaseVoltageKindParameter, 2.0));
                const bool delta = std::get<unsigned>(value) != 0;
                i.definition = delta ? kThreePhaseDeltaDefinition : kThreePhaseYDefinition;
                set_instance_parameter(i, kThreePhaseVoltageKindParameter, kind);
                set_instance_parameter(i, kThreePhaseVoltageParameter,
                                       three_phase_internal_voltage(kind, delta, display));
                return;
            }
            if (key == "three_phase_voltage_kind") {
                set_instance_parameter(i, kThreePhaseVoltageKindParameter, std::get<unsigned>(value));
                return;
            }
            if (key == "three_phase_voltage") {
                const auto kind = unsigned(instance_parameter_value_or(p, i, kThreePhaseVoltageKindParameter, 2.0));
                const bool delta = three_phase_delta_definition(p, i.definition);
                set_instance_parameter(i, kThreePhaseVoltageParameter,
                                       three_phase_internal_voltage(kind, delta, std::get<double>(value)));
                return;
            }
        }
        if(key.rfind("parameter/",0)==0) {
            const auto parameter=key.substr(10);
            if (three_phase_source_definition(p, i.definition)) {
                if (parameter == kThreePhaseVoltageKindParameter) {
                    set_instance_parameter(i, kThreePhaseVoltageKindParameter,
                                           std::holds_alternative<unsigned>(value)
                                               ? double(std::get<unsigned>(value))
                                               : std::get<double>(value));
                    return;
                }
                if (parameter == kThreePhaseVoltageParameter) {
                    const auto kind = unsigned(instance_parameter_value_or(p, i, kThreePhaseVoltageKindParameter, 2.0));
                    const bool delta = three_phase_delta_definition(p, i.definition);
                    set_instance_parameter(i, kThreePhaseVoltageParameter,
                                           three_phase_internal_voltage(kind, delta, std::get<double>(value)));
                    return;
                }
            }
            (void)definition(p,i.definition);
            auto v=std::find_if(i.parameters.begin(),i.parameters.end(),[&](const auto& v){return v.first==parameter;});
            if(v==i.parameters.end())i.parameters.emplace_back(parameter,std::get<double>(value));else v->second=std::get<double>(value);
            return;
        }
    }
    if (key == "events") {
        std::erase_if(p.events, [&](const GateEvent &e) { return e.target == id; });
        for (auto e : std::get<std::vector<GateEvent>>(value)) {
            e.target = id;
            p.events.push_back(e);
        }
        return;
    }
    auto common = [&](auto &o) {
        if (key == "name") {
            o.name = std::get<std::string>(value);
            return true;
        }
        if (key == "x") {
            o.x = std::get<double>(value);
            return true;
        }
        if (key == "y") {
            o.y = std::get<double>(value);
            return true;
        }
        return false;
    };
    for (auto &c : p.components)
        if (c.id == id) {
            if (common(c))
                return;
            if (key == "value")
                c.value = std::get<double>(value);
            if (key == "initial")
                c.initial = std::get<double>(value);
            if (key == "parallel_resistance_enabled")
                c.parallel_resistance_enabled = std::get<bool>(value);
            if (key == "parallel_resistance")
                c.parallel_resistance = std::get<double>(value);
            if (key == "closed")
                c.closed = std::get<bool>(value);
            if(key=="initial_latched")c.semiconductor.initial_latched=std::get<bool>(value);
            if(key=="semiconductor_model")c.semiconductor.model=SemiconductorModel(std::get<unsigned>(value));
            if(key=="charge_model") {
                const auto enabled=std::get<unsigned>(value);
                if(enabled>1)throw Diagnostic("invalid_diode_charge",c.id,"Unknown charge model");
                c.semiconductor.charge_dynamics=enabled!=0;
            }
            if(auto v=semiconductor_parameter(c.semiconductor,key))*v=std::get<double>(value);
            if(key=="source_mode") {
                c.source.kind=Waveform(std::get<unsigned>(value));
                if(c.source.kind==Waveform::piecewise_linear&&c.source.points.empty())
                    c.source.points={{0,0},{.01,c.value}};
            }
            if(key=="source_points")c.source.points=std::get<std::vector<Point>>(value);
            if(key=="source_phase_deg") {
                c.source.phase=std::get<double>(value)*std::numbers::pi/180.0;
                return;
            }
            if(auto v=source_parameter(c.source,key))*v=std::get<double>(value);
            return;
        }
    for (auto &n : p.nodes)
        if (n.id == id) {
            common(n);
            return;
        }
    for (auto &t : p.tags)
        if (t.id == id) {
            if (common(t))
                return;
            if (key == "tag_domain")
                t.domain = Domain(std::get<unsigned>(value));
            if (key == "tag_scope")
                t.scope = TagScope(std::get<unsigned>(value));
            if (key == "tag_listed")
                t.listed = std::get<bool>(value);
            return;
        }
    for (auto &g : p.patterns)
        if (g.id == id) {
            if (common(g))
                return;
            if (key == "gate_mode") {
                const auto mode = std::get<unsigned>(value);
                if (mode > 2)
                    throw Diagnostic("invalid_gate_mode", id, "Unknown gate mode");
                g.pwm = mode == 1;
                g.script = mode == 2;
                if (g.pwm && g.frequency <= 0)
                    g.frequency = 1000;
                if (g.script && g.code.empty())
                    g.code = "pwm(1000, 0.5, 0)";
                if (g.pwm || g.script)
                    std::erase_if(p.events, [&](const GateEvent &e) { return e.target == id; });
                return;
            }
            if (key == "closed")
                g.initial = std::get<bool>(value);
            if (key == "frequency")
                g.frequency = std::get<double>(value);
            if (key == "duty")
                g.duty = std::get<double>(value);
            if (key == "delay")
                g.delay = std::get<double>(value);
            if (key == "gate_code")
                g.code = std::get<std::string>(value);
            if (key == "script_step")
                g.script_step = std::get<double>(value);
            return;
        }
    for (auto &g : p.plots)
        if (g.id == id) {
            if (common(g))
                return;
            if (key == "inputs") {
                g.inputs = std::get<unsigned>(value);
                std::erase_if(g.pin_positions, [&](const PinPosition &pin) {
                    for (unsigned i = 1; i <= g.inputs; ++i)
                        if ((!g.differential && pin.port == "in" + std::to_string(i)) ||
                            (g.differential && (pin.port == "p" + std::to_string(i) ||
                                                pin.port == "n" + std::to_string(i))))
                            return false;
                    return true;
                });
                std::erase_if(p.wires, [&](const Wire &w) {
                    for (const auto &e : {w.from, w.to})
                        if (e.object == id) {
                            for (unsigned i = 1; i <= g.inputs; ++i)
                                if ((!g.differential && e.port == "in" + std::to_string(i)) ||
                                    (g.differential && (e.port == "p" + std::to_string(i) ||
                                                        e.port == "n" + std::to_string(i))))
                                    return false;
                            return true;
                        }
                    return false;
                });
            }
            return;
        }
    for (auto &w : p.wires)
        if (w.id == id) {
            if (key == "wire_color") {
                const auto &color = std::get<std::string>(value);
                if (!color.empty() && (color.size() != 7 || color[0] != '#'))
                    throw Diagnostic("invalid_wire_style", id, "Expected #RRGGBB color");
                w.color = color;
            } else if (key == "wire_width") {
                const auto width = std::get<double>(value);
                if (!std::isfinite(width) || width < .5 || width > 10)
                    throw Diagnostic("invalid_wire_style", id, "Wire width must be from 0.5 to 10");
                w.width = width;
            } else if (key == "wire_line") {
                const auto line = std::get<unsigned>(value);
                if (line > unsigned(WireLine::dash))
                    throw Diagnostic("invalid_wire_style", id, "Unknown wire line style");
                w.line = WireLine(line);
            }
            return;
        }
}
} // namespace pds
