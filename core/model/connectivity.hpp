#pragma once
#include "core/model/model.hpp"
#include <map>
namespace pds {
struct ResolvedGraph { Project project; std::map<std::string,std::string> nets; };
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
