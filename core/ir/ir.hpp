#pragma once
#include "core/model/model.hpp"
#include <utility>
namespace pds {
// Backend-independent branch stamps. Node -1 denotes electrical reference.
struct Stamp { Component component; int positive=-1, negative=-1, branch=-1; };
struct Channel { std::string object, name, unit; };
struct Observation { Channel channel; int positive=-1,negative=-1; double gain=1,offset=0; };
struct GateSignal { std::string id,name; bool initial=false; };
struct SimulationIR {
    std::string project_id;
    Profile profile;
    std::vector<Stamp> stamps;
    std::vector<GateEvent> events;
    std::vector<Channel> unknowns;
    std::vector<Observation> observations;
    std::vector<GateSignal> gate_signals;
    std::vector<std::pair<int,int>> sparsity;
    int node_count=0;
};
SimulationIR compile(const Project& project);
}