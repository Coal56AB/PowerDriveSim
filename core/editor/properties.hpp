#pragma once
#include "core/model/model.hpp"
#include <variant>
namespace pds {
using PropertyValue =
    std::variant<std::string, double, bool, unsigned, std::vector<GateEvent>, std::vector<Point>>;
std::string object_type(const Project &, const std::string &id);
PropertyValue read_property(const Project &, const std::string &id, const std::string &key);
void write_property(Project &, const std::string &id, const std::string &key, const PropertyValue &);
bool external_gate(const Project &, const std::string &id);
} // namespace pds
