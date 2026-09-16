#include "core/editor/properties.hpp"
#include "core/model/hierarchy.hpp"
#include "core/model/waveform.hpp"
#include "core/model/semiconductor.hpp"
#include <algorithm>
#include <optional>
namespace pds {
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
    for (const auto &g : p.patterns)
        if (g.id == id)
            return g.pwm ? "pwm" : "pattern";
    for (const auto &g : p.plots)
        if (g.id == id)
            return "plot";
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
        if(key.rfind("parameter/",0)==0) {
            const auto parameter=key.substr(10);
            for(const auto& param:definition(p,i.definition).parameters)if(param.id==parameter) {
                for(const auto& override:i.parameters)if(override.first==parameter)return override.second;
                return param.value;
            }
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
            if (key == "closed")
                return c.closed;
            if(c.kind==Kind::diode||c.kind==Kind::ideal_switch) {
                if(key=="semiconductor_model")return unsigned(c.semiconductor.model);
                if(c.kind==Kind::diode||key!="forward_voltage")
                    if(auto v=semiconductor_parameter(c.semiconductor,key))return *v;
            }
            if(c.kind==Kind::voltage||c.kind==Kind::current) {
                if(key=="source_mode")return unsigned(c.source.kind);
                if(key=="source_points")return c.source.points;
                if(auto v=source_parameter(c.source,key))return *v;
            }
        }
    for (const auto &n : p.nodes)
        if (n.id == id)
            if (auto v = common(n))
                return *v;
    for (const auto &g : p.patterns)
        if (g.id == id) {
            if (auto v = common(g))
                return *v;
            if (key == "closed")
                return g.initial;
            if (key == "frequency")
                return g.frequency;
            if (key == "duty")
                return g.duty;
            if (key == "delay")
                return g.delay;
        }
    for (const auto &g : p.plots)
        if (g.id == id) {
            if (auto v = common(g))
                return *v;
            if (key == "inputs")
                return g.inputs;
        }
    for (const auto &w : p.wires)
        if (w.id == id && key == "bends")
            return w.bends;
    throw Diagnostic("unknown_property", id, "Unsupported property: " + key);
}
void write_property(Project &p, const std::string &id, const std::string &key, const PropertyValue &value) {
    (void)read_property(p, id, key); // Validate the binding before mutation.
    for(auto& i:p.instances)if(i.id==id) {
        if(key=="name"){i.name=std::get<std::string>(value);return;}
        if(key=="x"){i.x=std::get<double>(value);return;}
        if(key=="y"){i.y=std::get<double>(value);return;}
        if(key.rfind("parameter/",0)==0) {
            const auto parameter=key.substr(10);auto v=std::find_if(i.parameters.begin(),i.parameters.end(),[&](const auto& v){return v.first==parameter;});
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
            if (key == "closed")
                c.closed = std::get<bool>(value);
            if(key=="semiconductor_model")c.semiconductor.model=SemiconductorModel(std::get<unsigned>(value));
            if(auto v=semiconductor_parameter(c.semiconductor,key))*v=std::get<double>(value);
            if(key=="source_mode") {
                c.source.kind=Waveform(std::get<unsigned>(value));
                if(c.source.kind==Waveform::piecewise_linear&&c.source.points.empty())
                    c.source.points={{0,0},{.01,c.value}};
            }
            if(key=="source_points")c.source.points=std::get<std::vector<Point>>(value);
            if(auto v=source_parameter(c.source,key))*v=std::get<double>(value);
            return;
        }
    for (auto &n : p.nodes)
        if (n.id == id) {
            common(n);
            return;
        }
    for (auto &g : p.patterns)
        if (g.id == id) {
            if (common(g))
                return;
            if (key == "closed")
                g.initial = std::get<bool>(value);
            if (key == "frequency")
                g.frequency = std::get<double>(value);
            if (key == "duty")
                g.duty = std::get<double>(value);
            if (key == "delay")
                g.delay = std::get<double>(value);
            return;
        }
    for (auto &g : p.plots)
        if (g.id == id) {
            if (common(g))
                return;
            if (key == "inputs") {
                g.inputs = std::get<unsigned>(value);
                std::erase_if(p.wires, [&](const Wire &w) {
                    for (const auto &e : {w.from, w.to})
                        if (e.object == id) {
                            for (unsigned i = 1; i <= g.inputs; ++i)
                                if (e.port == "in" + std::to_string(i))
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
            w.bends = std::get<std::vector<Point>>(value);
            return;
        }
}
} // namespace pds
