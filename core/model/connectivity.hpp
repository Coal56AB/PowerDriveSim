#pragma once
#include "core/model/model.hpp"
#include <map>
namespace pds {
struct ResolvedGraph { Project project; std::map<std::string,std::string> nets; };
std::vector<std::string> plot_channels(const Project& project,const std::string& plot_id);
std::string endpoint_key(const Endpoint& endpoint);
PortType port_type(const Project& project,const Endpoint& endpoint);
void validate_wire(const Project& project,const Wire& wire);
ResolvedGraph resolve_connections(const Project& project);
Project make_wired(const Project& project);
}
