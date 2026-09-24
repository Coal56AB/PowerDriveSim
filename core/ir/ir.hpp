#pragma once
#include "core/model/model.hpp"
#include "core/ir/signal.hpp"
#include <utility>
#include <map>
namespace pds {
// Backend-independent branch stamps. Node -1 denotes electrical reference.
struct Stamp {
    Component component;
    int positive=-1, negative=-1, branch=-1;
    int secondary_positive=-1, secondary_negative=-1, secondary_branch=-1;
    int mechanical=-1;
};
struct Channel { std::string object, name, unit; };
struct Observation { Channel channel; int positive=-1,negative=-1; double gain=1,offset=0; int source_stamp=-1; };
struct GateSignal { std::string id,name; bool initial=false; };
struct GateProgramOutput {
    std::size_t signal=0;
    std::vector<std::size_t> targets;
};
struct GateProgram {
    std::string id, source;
    std::vector<GateProgramOutput> outputs;
};
enum class SignalInputSource { unknown, observation, gate };
struct SignalInputBinding {
    SignalEndpointIR endpoint;
    SignalScalarType type=SignalScalarType::real;
    std::string unit;
    SignalInputSource source=SignalInputSource::unknown;
    std::size_t index=0;
};
struct SignalGateBinding {
    SignalEndpointIR endpoint;
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
    SignalIR signal;
    std::vector<SignalInputBinding> signal_inputs;
    std::vector<SignalGateBinding> signal_gates;
    std::vector<std::pair<int,int>> sparsity;
    int node_count=0;
};
SimulationIR compile(const Project& project);
}
