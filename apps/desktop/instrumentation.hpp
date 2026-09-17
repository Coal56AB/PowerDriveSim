#pragma once

#include "core/model/model.hpp"
#include <algorithm>
#include <optional>
#include <string>

namespace pds::desktop {

inline constexpr const char *hidden_current_probe_prefix = "x-hidden-current-probe ";

inline std::string hidden_current_probe_record(const std::string &id) {
    return std::string(hidden_current_probe_prefix) + id;
}

inline bool is_hidden_current_probe(const Schematic &schematic, const std::string &id) {
    const auto record = hidden_current_probe_record(id);
    return std::find(schematic.extensions.begin(), schematic.extensions.end(), record) !=
           schematic.extensions.end();
}

inline void mark_hidden_current_probe(Schematic &schematic, const std::string &id) {
    const auto record = hidden_current_probe_record(id);
    if (std::find(schematic.extensions.begin(), schematic.extensions.end(), record) ==
        schematic.extensions.end())
        schematic.extensions.push_back(record);
}

inline void unmark_hidden_current_probe(Schematic &schematic, const std::string &id) {
    const auto record = hidden_current_probe_record(id);
    std::erase(schematic.extensions, record);
}

struct HiddenCurrentWireView {
    std::string probe;
    std::string primary;
    std::string secondary;
    Endpoint from;
    Endpoint to;
    std::vector<Point> bends;
};

inline std::optional<HiddenCurrentWireView> hidden_current_wire_view(const Schematic &schematic,
                                                                     const std::string &wire_id) {
    for (const auto &component : schematic.components) {
        if (component.kind != Kind::current_probe || !is_hidden_current_probe(schematic, component.id))
            continue;
        const Wire *primary = nullptr, *secondary = nullptr;
        auto probe_port = [&](const Wire &wire) -> std::string {
            if (wire.from.object == component.id)
                return wire.from.port;
            if (wire.to.object == component.id)
                return wire.to.port;
            return {};
        };
        for (const auto &wire : schematic.wires) {
            const auto port = probe_port(wire);
            if (port == "p")
                primary = &wire;
            else if (port == "n")
                secondary = &wire;
        }
        if (!primary || !secondary || (wire_id != primary->id && wire_id != secondary->id))
            continue;
        auto external = [&](const Wire &wire) {
            return wire.from.object == component.id ? wire.to : wire.from;
        };
        return HiddenCurrentWireView{component.id, primary->id, secondary->id,
                                     external(*primary), external(*secondary), primary->bends};
    }
    return {};
}

} // namespace pds::desktop
