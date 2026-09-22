#pragma once
#include "core/model/model.hpp"
#include <map>
namespace pds {
struct ResolvedGraph {
    Project project;
    std::map<std::string,std::string> nets;
    // Controlled component UUID -> connected gate-source UUID. Kept as
    // metadata after patterns are removed from the electrical project.
    std::map<std::string,std::string> gate_drivers;
    // Code-block input endpoint key -> source endpoint. Kept independently so
    // different ports of one block can each have their own driver.
    std::map<std::string,Endpoint> signal_drivers;
};
std::vector<std::string> plot_channels(const Project& project,const std::string& plot_id);
std::vector<std::string> plot_source_channels(const Project& project,const std::string& plot_id);
std::vector<std::pair<std::string,std::string>> plot_differential_channels(const Project& project,
                                                                           const std::string& plot_id);
std::string endpoint_key(const Endpoint& endpoint);
PortType port_type(const Project& project,const Endpoint& endpoint);
void validate_wire(const Project& project,const Wire& wire);
// Origins are optional metadata for an already flattened project. Display names
// prefer explicit labels at the shallowest level; net identities remain stable.
ResolvedGraph resolve_connections(const Project& project,
                                  const std::map<std::string,ObjectPath>& origins = {});
Project make_wired(const Project& project);
}
