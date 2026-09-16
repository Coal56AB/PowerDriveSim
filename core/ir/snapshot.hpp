#pragma once
#include "core/ir/ir.hpp"
#include <cstdint>
namespace pds {
// Numeric state aligned with a validated IR. No backend handles or factorization
// caches are persisted. History is the conjugate C/L quantity (or diode dq/dt).
struct SimulationSnapshot {
    unsigned version = 1;
    std::string project_id, contract;
    double time = 0;
    std::uint64_t next_grid = 1;
    std::vector<double> states, history, values;
    std::vector<bool> gates, diodes, latched, signal_values;
    bool operator==(const SimulationSnapshot &) const = default;
};
// Geometry, names, recording selection and stop time do not affect compatibility.
// Already applied gate events are part of the contract; future events may change.
std::string snapshot_contract(const SimulationIR &ir, double time);
void validate_snapshot(const SimulationSnapshot &snapshot, const SimulationIR &ir);
} // namespace pds
