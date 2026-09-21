#pragma once
#include "core/model/model.hpp"
#include <utility>
#include <map>
namespace pds {
// Backend-independent branch stamps. Node -1 denotes electrical reference.
struct Stamp { Component component; int positive=-1, negative=-1, branch=-1; };
struct Channel { std::string object, name, unit; };
struct Observation { Channel channel; int positive=-1,negative=-1; double gain=1,offset=0; int source_stamp=-1; };
struct GateSignal { std::string id,name; bool initial=false; };
struct GateProgram {
    std::string id, source;
    std::vector<std::size_t> targets;
};
struct SimulationIR {
    std::map<std::string,ObjectPath> origins;
    std::string project_id;
    Profile profile;
    std::vector<Stamp> stamps;
    std::vector<GateEvent> events;
    std::vector<Channel> unknowns;
    std::vector<Observation> observations;
    std::vector<GateSignal> gate_signals;
    std::vector<GateProgram> gate_programs;
    std::vector<std::pair<int,int>> sparsity;
    int node_count=0;
};
SimulationIR compile(const Project& project);
}
