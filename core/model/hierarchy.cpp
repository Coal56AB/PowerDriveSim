#include "core/model/hierarchy.hpp"
#include "core/model/expression.hpp"
#include "core/model/waveform.hpp"
#include "core/model/semiconductor.hpp"
#include "core/model/connectivity.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <numbers>
#include <optional>
#include <set>
#include <utility>

namespace pds {
void remap_view_options(ViewOptions &view, const std::map<std::string, std::string> &identities) {
    auto remap = [&](std::string &id) {
        if (auto item = identities.find(id); item != identities.end())
            id = item->second;
    };
    remap(view.plot);
    remap(view.cursor_channel_a);
    remap(view.cursor_channel_b);
    for (auto &key : view.hidden_channels)
        remap(key);
    for (auto &style : view.curve_styles)
        remap(style.channel);
    for (auto &[key, multiplier] : view.curve_multipliers) {
        (void)multiplier;
        remap(key);
    }
    for (auto &[key, display] : view.signal_displays) {
        (void)display;
        remap(key);
    }
    for (auto &[key, name] : view.curve_names) {
        (void)name;
        remap(key);
    }
}
const Definition &definition(const Project &p, const std::string &id) {
    auto found =
        std::find_if(p.definitions.begin(), p.definitions.end(), [&](const auto &d) { return d.id == id; });
    if (found == p.definitions.end())
        throw Diagnostic("missing_definition", id, "Subcircuit definition does not exist");
    return *found;
}
Project definition_project(const Project &p, const std::string &id) {
    const auto &d = definition(p, id);
    Project body;
    static_cast<Schematic &>(body) = d;
    body.id = d.id;
    body.name = d.name;
    body.profile = p.profile;
    body.definitions = p.definitions;
    return body;
}
bool public_parameter_accepts(const PublicParameter &parameter, double value) {
    return std::isfinite(value) &&
           (!parameter.has_minimum || value >= parameter.minimum) &&
           (!parameter.has_maximum || value <= parameter.maximum);
}
std::string public_parameter_binding_key(const std::string &object, const std::string &field) {
    // Both phase units write to the same stored source phase.
    return object + "/" + (field == "source_phase_deg" ? "source_phase" : field);
}
std::string expanded_uuid(const std::vector<std::string> &path, const std::string &object) {
    auto id = object;
    // Compose from the leaf outwards so replacing a nested instance with its
    // expanded local UUIDs preserves the identities seen from every ancestor.
    for (auto i = path.rbegin(); i != path.rend(); ++i)
        id = derived_uuid("instance:" + *i + "/" + id);
    return id;
}
namespace {
void parameter_value(Schematic &s, const Project &catalog, const PublicParameter &p, double value) {
    if (!public_parameter_accepts(p, value))
        throw Diagnostic("invalid_public_parameter_value", p.id,
                         "Public parameter value is outside its configured range");
    if (p.object == "*") {
        if (p.field == "source_voltage_kind")
            return;
        bool applied = false;
        for (auto &c : s.components) {
            if (p.field == "value" && (c.kind == Kind::voltage || c.kind == Kind::current)) {
                c.value = value;
                applied = true;
            } else if (p.field == "three_phase_switch_ron" && c.kind == Kind::ideal_switch) {
                c.semiconductor.model = SemiconductorModel::piecewise_linear;
                c.semiconductor.ron = value;
                applied = true;
            } else if (p.field == "three_phase_switch_roff" && c.kind == Kind::ideal_switch) {
                c.semiconductor.model = SemiconductorModel::piecewise_linear;
                c.semiconductor.roff = value;
                applied = true;
            } else if (c.kind == Kind::voltage || c.kind == Kind::current) {
                if (p.field == "source_phase_offset") {
                    c.source.phase += value;
                    applied = true;
                } else if (auto target = source_parameter(c.source, p.field)) {
                    *target = value;
                    applied = true;
                }
            }
        }
        if (applied)
            return;
    }
    for (auto &c : s.components)
        if (c.id == p.object) {
            if (p.field == "value") {
                c.value = value;
                return;
            }
            if (p.field == "initial") {
                c.initial = value;
                return;
            }
            if (c.kind == Kind::inductor && p.field == "parallel_resistance") {
                c.parallel_resistance = value;
                return;
            }
            if ((c.kind == Kind::voltage || c.kind == Kind::current) &&
                p.field == "source_phase_deg") {
                c.source.phase = value * std::numbers::pi / 180.0;
                return;
            }
            if((c.kind==Kind::voltage||c.kind==Kind::current))
                if(auto target=source_parameter(c.source,p.field)){*target=value;return;}
            if(semiconductor_property(c.kind,p.field))
                if(auto target=semiconductor_parameter(c.semiconductor,p.field)){*target=value;return;}
        }
    for (auto &g : s.patterns)
        if (g.id == p.object && g.pwm) {
            if (p.field == "frequency") {
                g.frequency = value;
                return;
            }
            if (p.field == "duty") {
                g.duty = value;
                return;
            }
            if (p.field == "delay") {
                g.delay = value;
                return;
            }
        }
    for (auto &i : s.instances)
        if (i.id == p.object) {
            const auto &d = definition(catalog, i.definition);
            const auto target = std::find_if(d.parameters.begin(), d.parameters.end(),
                                             [&](const auto &v) { return v.id == p.field; });
            if (target == d.parameters.end())
                break;
            if (!public_parameter_accepts(*target, value))
                throw Diagnostic("invalid_public_parameter_value", p.id,
                                 "Public parameter value is outside the nested parameter range");
            // The nested parameter may itself expose a parameter of another
            // instance. Validate the complete binding chain even when this
            // definition is not instantiated by the root schematic.
            Schematic nested = d;
            parameter_value(nested, catalog, *target, value);
            auto v = std::find_if(i.parameters.begin(), i.parameters.end(),
                                  [&](const auto &v) { return v.first == p.field; });
            if (v == i.parameters.end())
                i.parameters.emplace_back(p.field, value);
            else
                v->second = value;
            return;
        }
    throw Diagnostic("invalid_parameter_binding", p.id,
                     "Public parameter refers to an unsupported numeric field");
}
Schematic configured_instance_body(const Project &catalog, const Instance &instance) {
    const auto &definition_body = definition(catalog, instance.definition);
    Schematic body = definition_body;
    for (const auto &parameter : definition_body.parameters) {
        const auto override = std::find_if(instance.parameters.begin(), instance.parameters.end(),
                                           [&](const auto &entry) { return entry.first == parameter.id; });
        parameter_value(body, catalog, parameter,
                        override == instance.parameters.end()
                            ? public_parameter_default_value(definition_body, parameter)
                            : override->second);
    }
    return body;
}
void validate_effective_components(const Schematic &schematic) {
    for (const auto &component : schematic.components) {
        validate_waveform(component);
        validate_semiconductor(component);
        if (component.parallel_resistance_enabled &&
            (component.kind != Kind::inductor || !std::isfinite(component.parallel_resistance) ||
             component.parallel_resistance <= 0))
            throw Diagnostic("invalid_parameter", component.id,
                             "Parallel resistance must be a positive inductor parameter");
    }
}
void validate_schematic(const Project &p) {
    std::set<std::string> ids;
    auto uuid = [&](const std::string &id) {
        if (!valid_uuid(id) || !ids.insert(id).second)
            throw Diagnostic("invalid_uuid", id, "Invalid or duplicate local UUID");
    };
    uuid(p.id);
    auto objects = [&](const auto &list) {
        for (const auto &o : list) {
            uuid(o.id);
            if (!std::isfinite(o.x) || !std::isfinite(o.y) || o.orientation.quarter_turns > 3 ||
                !std::isfinite(o.orientation.scale) || o.orientation.scale <= 0 ||
                !std::isfinite(o.orientation.scale_x) || o.orientation.scale_x <= 0 ||
                !std::isfinite(o.orientation.scale_y) || o.orientation.scale_y <= 0)
                throw Diagnostic("invalid_geometry", o.id, "Invalid object geometry");
        }
    };
    objects(p.components);
    validate_effective_components(p);
    objects(p.nodes);
    objects(p.tags);
    objects(p.patterns);
    objects(p.plots);
    objects(p.code_blocks);
    objects(p.instances);
    for (const auto &i : p.instances) {
        const auto &d = definition(p, i.definition);
        std::set<std::string> parameters;
        for (const auto &[key, value] : i.parameters) {
            const auto parameter = std::find_if(d.parameters.begin(), d.parameters.end(),
                                                [&](const auto &v) { return v.id == key; });
            if (!parameters.insert(key).second || parameter == d.parameters.end() ||
                !public_parameter_accepts(*parameter, value))
                throw Diagnostic("invalid_instance_parameter", i.id,
                                 "Unknown, duplicate or out-of-range parameter override");
            Schematic nested = d;
            parameter_value(nested, p, *parameter, value);
        }
    }
    if (!p.instances.empty() && !p.wired)
        throw Diagnostic("invalid_wiring", p.id, "Subcircuits require explicit wires");
    for (const auto &w : p.wires) {
        uuid(w.id);
        validate_wire(p, w);
    }
}
Point transform(Point p, Point offset, Orientation o) {
    p.x *= o.scale * o.scale_x;
    p.y *= o.scale * o.scale_y;
    for (unsigned n = 0; n < o.quarter_turns; ++n)
        p = {-p.y, p.x};
    if (o.mirrored)
        p.x = -p.x;
    return {p.x + offset.x, p.y + offset.y};
}
Orientation compose(Orientation parent, Orientation child) {
    int turns =
        int(child.quarter_turns) + (child.mirrored ? -int(parent.quarter_turns) : int(parent.quarter_turns));
    return {unsigned((turns + 4) % 4), parent.mirrored != child.mirrored, parent.scale * child.scale,
            parent.scale_x * child.scale_x, parent.scale_y * child.scale_y};
}
} // namespace
void validate_hierarchy(const Project &p) {
    if (p.definitions.empty() && p.instances.empty())
        return;
    std::map<std::string, unsigned> states;
    for (const auto &d : p.definitions) {
        if (!valid_uuid(d.id) || d.id == p.id || !states.emplace(d.id, 0).second)
            throw Diagnostic("invalid_uuid", d.id, "Invalid or duplicate definition UUID");
        if (!d.wired)
            throw Diagnostic("invalid_wiring", d.id, "A definition must use explicit wires");
    }
    std::function<void(const std::string &, size_t)> visit = [&](const std::string &id, size_t depth) {
        const auto &d = definition(p, id);
        if (states.at(id) == 1)
            throw Diagnostic("recursive_hierarchy", id, "Recursive subcircuit definition");
        if (depth > 64)
            throw Diagnostic("hierarchy_depth", id, "Hierarchy exceeds 64 levels");
        if (states.at(id) == 2)
            return;
        states[id] = 1;
        for (const auto &i : d.instances)
            visit(i.definition, depth + 1);
        states[id] = 2;
    };
    for (const auto &d : p.definitions)
        visit(d.id, 0);
    validate_schematic(p);
    for (const auto &d : p.definitions) {
        auto body = definition_project(p, d.id);
        validate_schematic(body);
        if(d.appearance.symbol < -1 || d.appearance.symbol > 9999 || d.appearance.image_png.size() > 8 * 1024 * 1024 ||
           std::any_of(d.appearance.image_png.begin(),d.appearance.image_png.end(),[](unsigned char c){
               return !(std::isalnum(c)||c=='+'||c=='/'||c=='=');
           }))
            throw Diagnostic("invalid_definition_appearance",d.id,"Definition appearance is invalid");
        std::set<std::string> ids, names, bindings;
        for (const auto &port : d.ports) {
            if (!valid_uuid(port.id) || !ids.insert(port.id).second || port.name.empty() ||
                !names.insert(port.name).second ||
                (port.has_position && (!std::isfinite(port.x) || !std::isfinite(port.y))))
                throw Diagnostic("invalid_public_port", port.id,
                                 "Public ports require unique UUIDs, names and finite positions");
            auto type = port_type(body, port.terminal);
            if (type.domain != port.domain || type.direction != port.direction)
                throw Diagnostic("incompatible_public_port", port.id,
                                 "Public port type must match its internal terminal");
        }
        names.clear();
        for (const auto &param : d.parameters) {
            const double default_value=public_parameter_default_value(d,param);
            if (!valid_uuid(param.id) || !ids.insert(param.id).second || param.name.empty() ||
                !names.insert(param.name).second ||
                !bindings.insert(public_parameter_binding_key(param.object, param.field)).second ||
                (param.has_minimum && !std::isfinite(param.minimum)) ||
                (param.has_maximum && !std::isfinite(param.maximum)) ||
                (param.has_minimum && param.has_maximum && param.minimum > param.maximum) ||
                !public_parameter_accepts(param,default_value))
                throw Diagnostic("invalid_public_parameter", param.id,
                                 "Public parameters require unique bindings and a valid value range");
            parameter_value(body,p,param,default_value);
        }
        if (!d.parameters.empty())
            validate_effective_components(body);
    }
    // Check effective instance values only after definition defaults and ranges.
    // This keeps invalid definition metadata as the first diagnostic.
    auto has_instance_expression = [](const Schematic &schematic) {
        return std::any_of(schematic.parameter_expressions.begin(), schematic.parameter_expressions.end(),
                           [](const ParameterExpression &expression) {
                               return expression.field.starts_with("parameter/");
                           });
    };
    std::optional<Project> resolved;
    if (has_instance_expression(p) ||
        std::any_of(p.definitions.begin(), p.definitions.end(), has_instance_expression))
        resolved = resolve_parameter_expressions(p);
    const Project &effective = resolved ? *resolved : p;
    std::function<void(const Instance &, std::vector<std::string>)> validate_instance =
        [&](const Instance &instance, std::vector<std::string> path) {
            if (definition(effective, instance.definition).parameters.empty())
                return;
            Schematic body;
            try {
                body = configured_instance_body(effective, instance);
                validate_effective_components(body);
            } catch (Diagnostic &diagnostic) {
                diagnostic.path = path;
                throw;
            }
            for (const auto &child : body.instances) {
                auto child_path = path;
                child_path.push_back(child.id);
                validate_instance(child, std::move(child_path));
            }
        };
    auto validate_instances = [&](const Schematic &schematic) {
        for (const auto &instance : schematic.instances)
            validate_instance(instance, {instance.id});
    };
    validate_instances(effective);
    for (const auto &definition_body : effective.definitions)
        validate_instances(definition_body);
}
FlattenedProject flatten(const Project &source) {
    validate_hierarchy(source);
    FlattenedProject result;
    result.project = source;
    result.project.definitions.clear();
    result.project.instances.clear();
    if (source.instances.empty()) {
        auto remember = [&](const auto &objects) {
            for (const auto &o : objects)
                result.origins.emplace(o.id, ObjectPath{{}, o.id});
        };
        remember(source.nodes);
        remember(source.components);
        remember(source.tags);
        remember(source.patterns);
        remember(source.plots);
        remember(source.code_blocks);
        remember(source.wires);
        return result;
    }
    static_cast<Schematic &>(result.project) = Schematic{};
    result.project.wired = true;
    result.project.extensions = source.extensions;
    size_t count = 0;
    std::map<std::string, std::map<std::string, Endpoint>> view_net_targets;
    std::function<std::map<std::string, Endpoint>(const Schematic &, const std::vector<std::string> &,
                                                  const std::string &, Point, Orientation)>
        expand;
    expand = [&](const Schematic &s, const std::vector<std::string> &path, const std::string &name,
                 Point offset, Orientation rotation) {
        if (path.size() > 64)
            throw Diagnostic("hierarchy_depth", path.back(), "Hierarchy exceeds 64 levels");
        std::map<std::string, Endpoint> endpoints;
        auto id = [&](const std::string &local) { return expanded_uuid(path, local); };
        auto remember = [&](const std::string &local) {
            if (++count > 1000000)
                throw Diagnostic("hierarchy_size", local, "Expanded circuit exceeds one million objects");
            if (!result.origins.emplace(id(local), ObjectPath{path, local}).second)
                throw Diagnostic("invalid_uuid", local, "Expanded UUID collision");
        };
        auto objects = [&](const auto &from, auto &to) {
            for (auto o : from) {
                remember(o.id);
                o.id = id(o.id);
                if (!o.name.empty())
                    o.name = name + o.name;
                auto pt = transform({o.x, o.y}, offset, rotation);
                o.x = pt.x;
                o.y = pt.y;
                o.orientation = compose(rotation, o.orientation);
                to.push_back(std::move(o));
            }
        };
        auto &flat = result.project;
        objects(s.nodes, flat.nodes);
        objects(s.components, flat.components);
        for (auto tag : s.tags) {
            remember(tag.id);
            tag.connection_name = tag.name;
            tag.scope_path = path;
            tag.id = id(tag.id);
            if (!tag.name.empty())
                tag.name = name + tag.name;
            auto pt = transform({tag.x, tag.y}, offset, rotation);
            tag.x = pt.x;
            tag.y = pt.y;
            tag.orientation = compose(rotation, tag.orientation);
            flat.tags.push_back(std::move(tag));
        }
        objects(s.patterns, flat.patterns);
        objects(s.plots, flat.plots);
        objects(s.code_blocks, flat.code_blocks);
        for(auto appearance:s.object_icons) {
            if(std::any_of(s.instances.begin(),s.instances.end(),[&](const Instance &instance) {
                return instance.id==appearance.object;
            }))
                continue; // Instances are recursively expanded and have no flat visual object.
            appearance.object=id(appearance.object);
            flat.object_icons.push_back(std::move(appearance));
        }
        auto add = [&](const std::string &object, const std::string &port) {
            endpoints[endpoint_key({object, port})] = {id(object), port};
        };
        for (const auto &n : s.nodes)
            add(n.id, "node");
        for (const auto &c : s.components) {
            add(c.id, "p");
            add(c.id, "n");
            if (gate_controlled(c.kind))
                add(c.id, "gate");
            if (c.kind == Kind::voltage_probe || c.kind == Kind::current_probe)
                add(c.id, "out");
        }
        for (const auto &g : s.patterns)
            for (unsigned index = 0; index < g.outputs; ++index)
                add(g.id, index == 0 ? "out" : "out" + std::to_string(index));
        for (const auto &t : s.tags)
            add(t.id, "io");
        for (const auto &p : s.plots)
            for (unsigned n = 1; n <= p.inputs; ++n)
                add(p.id, "in" + std::to_string(n));
        for (const auto &block : s.code_blocks) {
            for (const auto &port : block.inputs)
                add(block.id, port.id);
            for (const auto &port : block.outputs)
                add(block.id, port.id);
        }
        for (const auto &i : s.instances) {
            const auto &d = definition(source, i.definition);
            Schematic body = configured_instance_body(source, i);
            auto child_path = path;
            child_path.push_back(i.id);
            if (!d.parameters.empty()) {
                try {
                    validate_effective_components(body);
                } catch (Diagnostic &diagnostic) {
                    diagnostic.path = child_path;
                    throw;
                }
            }
            auto terminals =
                expand(body, child_path, name + i.name + "/", transform({i.x, i.y}, offset, rotation),
                       compose(rotation, i.orientation));
            for (const auto &port : d.ports) {
                const auto terminal = terminals.at(endpoint_key(port.terminal));
                endpoints[endpoint_key({i.id, port.id})] = terminal;
                result.terminals[endpoint_key({id(i.id), port.id})] = terminal;
            }
        }
        for (auto wire : s.wires) {
            remember(wire.id);
            wire.id = id(wire.id);
            wire.from = endpoints.at(endpoint_key(wire.from));
            wire.to = endpoints.at(endpoint_key(wire.to));
            for (auto &pt : wire.bends)
                pt = transform(pt, offset, rotation);
            flat.wires.push_back(std::move(wire));
        }
        for (auto e : s.events) {
            e.target = id(e.target);
            flat.events.push_back(e);
        }
        for (auto label : s.labels) {
            label.object = id(label.object);
            flat.labels.push_back(label);
        }
        auto channel = [&](std::string &key) {
            if (key.empty())
                return;
            if (key.rfind("gate/", 0) == 0)
                key = "gate/" + id(key.substr(5));
            else
                key = id(key);
        };
        std::map<std::string, Endpoint> net_targets;
        if (!path.empty() && !s.view_options.empty()) {
            // Resolve local implicit nets without recursively processing display settings.
            Project local = source;
            static_cast<Schematic &>(local) = s;
            local.id = derived_uuid("view-schematic:" + id(source.id));
            local.view_options.clear();
            for (auto &d : local.definitions)
                d.view_options.clear();
            for (const auto &[key, net] : resolve_connections(local).nets) {
                const auto slash = key.find('/');
                Endpoint terminal{id(key.substr(0, slash)), key.substr(slash + 1)};
                if (auto alias = result.terminals.find(endpoint_key(terminal));
                    alias != result.terminals.end())
                    terminal = alias->second;
                net_targets[id(net)] = terminal;
            }
        }
        for (auto view : s.view_options) {
            if (view.plot.empty() && !path.empty())
                continue;
            if (!view.plot.empty())
                view.plot = id(view.plot);
            view_net_targets[view.plot] = net_targets;
            channel(view.cursor_channel_a);
            channel(view.cursor_channel_b);
            for (auto &[key, display] : view.signal_displays) {
                (void)display;
                channel(key);
            }
            for (auto &key : view.hidden_channels)
                channel(key);
            for (auto &style : view.curve_styles)
                channel(style.channel);
            for (auto &[key, alias] : view.curve_names) {
                (void)alias;
                channel(key);
            }
            auto prior = std::find_if(flat.view_options.begin(), flat.view_options.end(),
                                      [&](const auto &v) { return v.plot == view.plot; });
            if (prior == flat.view_options.end())
                flat.view_options.push_back(std::move(view));
            else
                *prior = std::move(view);
        }
        return endpoints;
    };
    expand(source, {}, "", {}, {});
    if (std::any_of(view_net_targets.begin(), view_net_targets.end(),
                    [](const auto &item) { return !item.second.empty(); })) {
        const auto nets = resolve_connections(result.project).nets;
        for (auto &view : result.project.view_options) {
            std::map<std::string, std::string> channels;
            for (const auto &[channel, terminal] : view_net_targets[view.plot])
                if (auto found = nets.find(endpoint_key(terminal)); found != nets.end())
                    channels[channel] = found->second;
            remap_view_options(view, channels);
        }
    }
    return result;
}
} // namespace pds
