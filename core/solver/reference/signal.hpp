#pragma once

#include "core/ir/signal.hpp"
#include "core/model/c_program.hpp"

#include <cstdint>

namespace pds {

struct SignalTaskRuntime {
    std::uint64_t next_tick = 0;
    CProgram program;
    CProgramState program_state;
    std::map<std::string, double> outputs;
};

struct SignalRuntimeState {
    std::map<std::string, SignalTaskRuntime> tasks;
    SignalFrame outputs;
};

struct SignalEmission {
    SignalEndpointIR endpoint;
    SignalValue value;
};

SignalRuntimeState initialize_signal_runtime(const SignalIR &ir);
std::vector<SignalEmission> run_signal_tasks(const SignalIR &ir, SignalRuntimeState &state,
                                             double through_time, const SignalFrame &accepted_inputs);

} // namespace pds
