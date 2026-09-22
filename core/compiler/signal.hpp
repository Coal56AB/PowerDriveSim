#pragma once

#include "core/ir/signal.hpp"

namespace pds {

// Expands hierarchy and resolves explicit probe, Gate and code-block links.
// The electrical compiler remains independent of this signal-only IR.
SignalIR compile_signal_ir(const Project &project);

} // namespace pds
