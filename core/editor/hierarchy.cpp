#include "core/model/hierarchy.hpp"
#include "core/editor/document.hpp"
#include <algorithm>
#include <set>
namespace pds {
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
    apply("Create subcircuit", [&](Project &p) {
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
        const auto before = resolve_connections(p).nets;
        std::map<std::string, std::string> public_ports;
        for (auto &wire : p.wires) {
            const bool from = ids.count(wire.from.object) != 0, to = ids.count(wire.to.object) != 0;
            if (from == to)
                continue;
            auto &endpoint = from ? wire.from : wire.to;
            auto key = endpoint_key(endpoint);
            if (!public_ports.count(key)) {
                auto type = port_type(p, endpoint);
                auto id = new_uuid();
                d.ports.push_back(
                    {id, "port" + std::to_string(d.ports.size() + 1), endpoint, type.domain, type.direction});
                public_ports[key] = id;
            }
            endpoint = {instance, public_ports.at(key)};
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
        const auto after = resolve_connections(p).nets;
        std::map<std::string, std::string> channels;
        for (const auto &id : ids) {
            channels[id] = expanded_uuid({instance}, id);
            channels["gate/" + id] = "gate/" + channels[id];
        }
        for (const auto &[endpoint, net] : before) {
            const auto slash = endpoint.find('/');
            const auto object = endpoint.substr(0, slash);
            const auto key =
                ids.count(object) ? expanded_uuid({instance}, object) + endpoint.substr(slash) : endpoint;
            if (auto found = after.find(key); found != after.end())
                channels[net] = found->second;
        }
        for (auto &key : p.scope_channels)
            if (channels.count(key))
                key = channels.at(key);
        for (auto &view : p.view_options) {
            auto remap = [&](std::string &key) {
                if (channels.count(key))
                    key = channels.at(key);
            };
            remap(view.cursor_channel_a);
            remap(view.cursor_channel_b);
            for (auto &key : view.hidden_channels)
                remap(key);
            for (auto &style : view.curve_styles)
                remap(style.channel);
            for (auto &[key, value] : view.signal_displays) {
                (void)value;
                remap(key);
            }
            for (auto &[key, value] : view.curve_names) {
                (void)value;
                remap(key);
            }
        }
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
        append(p.view_options, expanded.project.view_options);
        std::erase_if(p.instances, [&](const auto &i) { return i.id == id; });
        std::erase_if(p.labels, [&](const auto &l) { return l.object == id; });
    });
}
} // namespace pds
