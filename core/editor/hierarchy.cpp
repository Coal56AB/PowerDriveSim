#include "core/model/hierarchy.hpp"
#include "core/editor/document.hpp"
#include "core/editor/properties.hpp"
#include <algorithm>
#include <set>
namespace pds {
namespace {
std::string definition_at(const Project &p, const std::vector<std::string> &path) {
    const Schematic *level = &p;
    std::string id;
    for (const auto &step : path) {
        auto i = std::find_if(level->instances.begin(), level->instances.end(),
                              [&](const auto &instance) { return instance.id == step; });
        if (i == level->instances.end())
            throw Diagnostic("missing_instance", step, "Hierarchy path no longer exists");
        id = i->definition;
        level = &definition(p, id);
    }
    return id;
}
} // namespace
std::string Document::current_definition() const {
    return definition_at(current_, location_);
}
void Document::rebuild_view() {
    while (!location_.empty()) {
        try {
            const auto id = current_definition();
            view_ = definition_project(current_, id);
            view_.id = derived_uuid("definition-editor:" + id);
            for (const auto &param : definition(current_, id).parameters)
                write_property(view_, param.object,
                               valid_uuid(param.field) ? "parameter/" + param.field : param.field,
                               param.value);
            view_.scope_enabled = current_.scope_enabled;
            view_.scope_channels = current_.scope_channels;
            view_.scope_begin = current_.scope_begin;
            view_.scope_end = current_.scope_end;
            view_.cursor_a = current_.cursor_a;
            view_.cursor_b = current_.cursor_b;
            for (const auto &options : current_.view_options)
                if (options.plot.empty())
                    view_.view_options.push_back(options);
            return;
        } catch (const Diagnostic &) {
            location_.pop_back();
        }
    }
    view_ = Project{};
}
Project Document::merge_view(const Project &view) const {
    if (location_.empty())
        return view;
    auto result = current_;
    result.definitions = view.definitions;
    auto d = std::find_if(result.definitions.begin(), result.definitions.end(),
                          [&](const auto &item) { return item.id == current_definition(); });
    if (d == result.definitions.end())
        throw Diagnostic("missing_definition", current_definition(),
                         "Cannot remove the definition being edited");
    if (static_cast<const Schematic &>(view) != static_cast<const Schematic &>(view_)) {
        static_cast<Schematic &>(*d) = view;
        std::erase_if(d->view_options, [](const auto &options) { return options.plot.empty(); });
        for (auto &param : d->parameters) {
            const auto &old_parameters = definition(current_, current_definition()).parameters;
            auto old = std::find_if(old_parameters.begin(), old_parameters.end(),
                                    [&](const auto &old) { return old.id == param.id; });
            if (old == old_parameters.end() || old->object != param.object || old->field != param.field)
                continue;
            const auto key = valid_uuid(param.field) ? "parameter/" + param.field : param.field;
            if (read_property(view, param.object, key) != read_property(view_, param.object, key))
                param.value = std::get<double>(read_property(view, param.object, key));
        }
    }
    if (view.name != view_.name)
        d->name = view.name;
    result.profile = view.profile;
    result.scope_enabled = view.scope_enabled;
    result.scope_channels = view.scope_channels;
    result.scope_begin = view.scope_begin;
    result.scope_end = view.scope_end;
    result.cursor_a = view.cursor_a;
    result.cursor_b = view.cursor_b;
    for (const auto &options : view.view_options)
        if (options.plot.empty()) {
            auto found = std::find_if(result.view_options.begin(), result.view_options.end(),
                                      [](const auto &item) { return item.plot.empty(); });
            if (found == result.view_options.end())
                result.view_options.push_back(options);
            else
                *found = options;
        }
    return result;
}
void Document::navigate(const std::vector<std::string> &path) {
    (void)definition_at(current_, path);
    if (path == location_)
        return;
    undo_.push_back({"Navigate hierarchy", current_, current_, location_, path});
    if (undo_.size() > 100)
        undo_.erase(undo_.begin());
    redo_.clear();
    location_ = path;
    rebuild_view();
}
std::string Document::add_instance(const std::string &type, double x, double y) {
    const auto id = new_uuid();
    apply("Add subcircuit",
          [&](Project &p) { p.instances.push_back({id, definition(p, type).name, type, x, y}); });
    return id;
}
void Document::edit_definition(const std::string &id, const std::function<void(Definition &)> &change) {
    apply("Edit definition", [&](Project &p) {
        for (auto &d : p.definitions)
            if (d.id == id) {
                change(d);
                if (d.id != id)
                    throw Diagnostic("invalid_uuid", id, "Definition identity cannot change during editing");
                return;
            }
        throw Diagnostic("missing_definition", id, "Definition no longer exists");
    });
}
std::string Document::create_definition(const std::vector<std::string> &selected, const std::string &name) {
    const auto instance = new_uuid();
    std::map<std::string, std::string> channels;
    const auto original_views = current_.view_options;
    apply_with_root(
        "Create subcircuit",
        [&](Project &p) {
            std::set<std::string> ids(selected.begin(), selected.end());
            Definition d;
            d.id = new_uuid();
            d.name = name;
            d.wired = true;
            double x = 0, y = 0;
            size_t count = 0;
            auto center = [&](const auto &objects) {
                for (const auto &o : objects)
                    if (ids.count(o.id)) {
                        x += o.x;
                        y += o.y;
                        ++count;
                    }
            };
            center(p.nodes);
            center(p.components);
            center(p.patterns);
            center(p.plots);
            center(p.instances);
            if (!count || name.empty())
                throw Diagnostic("invalid_selection", p.id, "Select objects and supply a subcircuit name");
            x /= count;
            y /= count;
            const auto before = resolve_connections(current_).nets;
            const auto original = flatten(current_);
            std::map<std::string, std::string> public_ports;
            auto expose = [&](Endpoint endpoint) {
                auto key = endpoint_key(endpoint);
                if (!public_ports.count(key)) {
                    auto type = port_type(p, endpoint);
                    auto id = new_uuid();
                    auto port_name = endpoint.port;
                    for (const auto &nested : p.instances)
                        if (nested.id == endpoint.object)
                            for (const auto &port : definition(p, nested.definition).ports)
                                if (port.id == endpoint.port)
                                    port_name = port.name;
                    auto base =
                        std::get<std::string>(read_property(p, endpoint.object, "name")) + "." + port_name;
                    port_name = base;
                    unsigned suffix = 2;
                    while (std::any_of(d.ports.begin(), d.ports.end(),
                                       [&](const auto &port) { return port.name == port_name; }))
                        port_name = base + std::to_string(suffix++);
                    d.ports.push_back({id, port_name, endpoint, type.domain, type.direction});
                    public_ports[key] = id;
                }
                return Endpoint{instance, public_ports.at(key)};
            };
            for (auto &parent : p.definitions)
                if (parent.id == current_definition()) {
                    for (auto &port : parent.ports)
                        if (ids.count(port.terminal.object))
                            port.terminal = expose(port.terminal);
                    for (auto &param : parent.parameters)
                        if (ids.count(param.object)) {
                            d.parameters.push_back(param);
                            param.object = instance;
                            param.field = d.parameters.back().id;
                        }
                }
            for (auto &wire : p.wires) {
                const bool from = ids.count(wire.from.object) != 0, to = ids.count(wire.to.object) != 0;
                if (from == to)
                    continue;
                auto &endpoint = from ? wire.from : wire.to;
                endpoint = expose(endpoint);
                wire.bends.clear();
            }
            auto move = [&](auto &from, auto &to) {
                for (auto o : from)
                    if (ids.count(o.id)) {
                        o.x -= x;
                        o.y -= y;
                        to.push_back(std::move(o));
                    }
                std::erase_if(from, [&](const auto &o) { return ids.count(o.id); });
            };
            move(p.nodes, d.nodes);
            move(p.components, d.components);
            move(p.patterns, d.patterns);
            move(p.plots, d.plots);
            move(p.instances, d.instances);
            for (auto wire : p.wires)
                if (ids.count(wire.from.object) && ids.count(wire.to.object)) {
                    for (auto &pt : wire.bends) {
                        pt.x -= x;
                        pt.y -= y;
                    }
                    d.wires.push_back(std::move(wire));
                }
            std::erase_if(p.wires,
                          [&](const auto &w) { return ids.count(w.from.object) && ids.count(w.to.object); });
            for (const auto &e : p.events)
                if (ids.count(e.target))
                    d.events.push_back(e);
            std::erase_if(p.events, [&](const auto &e) { return ids.count(e.target); });
            for (const auto &l : p.labels)
                if (ids.count(l.object))
                    d.labels.push_back(l);
            std::erase_if(p.labels, [&](const auto &l) { return ids.count(l.object); });
            for (const auto &v : p.view_options)
                if (ids.count(v.plot))
                    d.view_options.push_back(v);
            std::erase_if(p.view_options, [&](const auto &v) { return ids.count(v.plot); });
            p.instances.push_back({instance, name, d.id, x, y});
            p.definitions.push_back(std::move(d));
            for (auto &parent : p.definitions)
                if (parent.id == current_definition())
                    static_cast<Schematic &>(parent) = p;
            const auto after = resolve_connections(merge_view(p)).nets;
            const auto edited_definition = current_definition();
            for (const auto &[id, origin] : original.origins) {
                auto path = origin.instances;
                std::vector<std::string> prefix;
                for (size_t level = 0; level <= origin.instances.size(); ++level) {
                    if (definition_at(current_, prefix) == edited_definition) {
                        const auto candidate =
                            level < origin.instances.size() ? origin.instances[level] : origin.object;
                        if (ids.count(candidate)) {
                            path.insert(path.begin() + static_cast<std::ptrdiff_t>(level), instance);
                            break;
                        }
                    }
                    if (level < origin.instances.size())
                        prefix.push_back(origin.instances[level]);
                }
                channels[id] = expanded_uuid(path, origin.object);
                channels["gate/" + id] = "gate/" + channels[id];
            }
            for (const auto &[endpoint, net] : before) {
                const auto slash = endpoint.find('/');
                const auto object = endpoint.substr(0, slash);
                const auto key =
                    channels.count(object) ? channels.at(object) + endpoint.substr(slash) : endpoint;
                if (auto found = after.find(key); found != after.end())
                    channels[net] = found->second;
            }
            for (auto &key : p.scope_channels)
                if (channels.count(key))
                    key = channels.at(key);
        },
        [&](Project &root) {
            root.view_options = original_views;
            for (auto &view : root.view_options)
                remap_view_options(view, channels);
        });
    return instance;
}
void Document::detach_instance(const std::string &id) {
    apply("Detach subcircuit", [&](Project &p) {
        auto instance =
            std::find_if(p.instances.begin(), p.instances.end(), [&](const auto &i) { return i.id == id; });
        if (instance == p.instances.end())
            throw Diagnostic("missing_instance", id, "Subcircuit no longer exists");
        std::map<std::string, std::string> copied;
        std::function<std::string(const std::string &)> clone = [&](const std::string &old) {
            if (copied.count(old))
                return copied.at(old);
            Definition d = definition(p, old);
            d.id = new_uuid();
            copied[old] = d.id;
            for (auto &child : d.instances)
                child.definition = clone(child.definition);
            auto fresh = d.id;
            p.definitions.push_back(std::move(d));
            return fresh;
        };
        instance->definition = clone(instance->definition);
    });
}
void Document::expand_instance(const std::string &id) {
    apply("Expand subcircuit", [&](Project &p) {
        auto instance =
            std::find_if(p.instances.begin(), p.instances.end(), [&](const auto &i) { return i.id == id; });
        if (instance == p.instances.end())
            throw Diagnostic("missing_instance", id, "Subcircuit no longer exists");
        Project part;
        part.id = p.id;
        part.profile = p.profile;
        part.wired = true;
        part.definitions = p.definitions;
        part.instances.push_back(*instance);
        auto expanded = flatten(part);
        for (auto &parent : p.definitions)
            if (parent.id == current_definition()) {
                for (auto &port : parent.ports)
                    if (port.terminal.object == id)
                        port.terminal = expanded.terminals.at(endpoint_key(port.terminal));
                for (auto &param : parent.parameters)
                    if (param.object == id) {
                        const Definition *target = &definition(p, instance->definition);
                        auto parameter = param.field;
                        std::vector<std::string> path{id};
                        for (;;) {
                            const auto binding =
                                std::find_if(target->parameters.begin(), target->parameters.end(),
                                             [&](const auto &v) { return v.id == parameter; });
                            if (binding == target->parameters.end())
                                throw Diagnostic("invalid_parameter_binding", param.id,
                                                 "Missing parameter while expanding instance");
                            const auto child =
                                std::find_if(target->instances.begin(), target->instances.end(),
                                             [&](const auto &i) { return i.id == binding->object; });
                            if (child == target->instances.end()) {
                                param.object = expanded_uuid(path, binding->object);
                                param.field = binding->field;
                                break;
                            }
                            path.push_back(child->id);
                            parameter = binding->field;
                            target = &definition(p, child->definition);
                        }
                    }
            }
        for (auto &wire : p.wires)
            for (auto *endpoint : {&wire.from, &wire.to})
                if (endpoint->object == id) {
                    *endpoint = expanded.terminals.at(endpoint_key(*endpoint));
                    wire.bends.clear();
                }
        auto append = [](auto &to, const auto &from) { to.insert(to.end(), from.begin(), from.end()); };
        append(p.nodes, expanded.project.nodes);
        append(p.components, expanded.project.components);
        append(p.patterns, expanded.project.patterns);
        append(p.plots, expanded.project.plots);
        append(p.wires, expanded.project.wires);
        append(p.events, expanded.project.events);
        append(p.labels, expanded.project.labels);
        for (const auto &view : expanded.project.view_options)
            if (std::none_of(p.view_options.begin(), p.view_options.end(),
                             [&](const auto &prior) { return prior.plot == view.plot; }))
                p.view_options.push_back(view);
        std::erase_if(p.instances, [&](const auto &i) { return i.id == id; });
        std::erase_if(p.labels, [&](const auto &l) { return l.object == id; });
    });
}
} // namespace pds
