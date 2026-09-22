#pragma once
#include "core/ir/ir.hpp"
#include "core/ir/signal.hpp"
#include "core/model/c_program.hpp"
#include <cstdint>
namespace pds {
struct SignalTaskSnapshot {
    std::uint64_t next_tick = 0;
    CProgramState program_state;
    std::map<std::string, double> outputs;
    bool operator==(const SignalTaskSnapshot &) const = default;
};
// Numeric state aligned with a validated IR. No backend handles or factorization
// caches are persisted. History is the conjugate C/L quantity (or diode dq/dt).
struct SimulationSnapshot {
    unsigned version = 3;
    std::string project_id, contract;
    double time = 0;
    std::uint64_t next_grid = 1;
    double next_step = 0; // Adaptive controller proposal; zero for fixed stepping.
    std::vector<double> states, history, values;
    std::vector<bool> gates, diodes, latched, signal_values;
    std::vector<CProgramState> gate_program_states;
    std::map<std::string, SignalTaskSnapshot> signal_tasks;
    SignalFrame signal_outputs, accepted_signal_inputs;
    bool operator==(const SimulationSnapshot &) const = default;
};
// Geometry, names, recording selection and stop time do not affect compatibility.
// Already applied gate events are part of the contract; future events may change.
std::string snapshot_contract(const SimulationIR &ir, double time);
void validate_snapshot(const SimulationSnapshot &snapshot, const SimulationIR &ir);
} // namespace pds
