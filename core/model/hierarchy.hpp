#pragma once
#include "core/model/model.hpp"
#include <map>
namespace pds {
struct FlattenedProject {
    Project project;
    std::map<std::string, ObjectPath> origins;
    std::map<std::string, Endpoint> terminals;
};
const Definition &definition(const Project &project, const std::string &id);
// Validate the entire catalog, including unused definitions and recursive references.
void validate_hierarchy(const Project &project);
// UUIDs depend on instance UUID paths, never on names, layout or definition UUIDs.
std::string expanded_uuid(const std::vector<std::string> &path, const std::string &object);
FlattenedProject flatten(const Project &project);
Project definition_project(const Project &project, const std::string &id);
bool public_parameter_accepts(const PublicParameter &parameter, double value);
// The two editable source phase units refer to one underlying numeric field.
std::string public_parameter_binding_key(const std::string &object, const std::string &field);
// Update plot and channel identities together after a graph editing operation.
void remap_view_options(ViewOptions &view, const std::map<std::string, std::string> &identities);
} // namespace pds
