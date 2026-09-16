#pragma once
#include "core/ir/ir.hpp"
#include <optional>
namespace pds {
// Constraints independent of switch/diode state can be rejected before execution.
void validate_source_loops(const SimulationIR &ir);
// Refine a failed linear solve; successful steps incur no topology-analysis cost.
std::optional<Diagnostic> diagnose_singular_topology(const SimulationIR &ir, bool initialize,
                                                     const std::vector<bool> &gates,
                                                     const std::vector<bool> &diodes,
                                                     const std::vector<double> &states, double time,
                                                     bool operating_point = false);
} // namespace pds
