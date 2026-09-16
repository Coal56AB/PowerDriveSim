#pragma once
#include "core/ir/snapshot.hpp"
#include <iosfwd>
namespace pds {
SimulationSnapshot read_snapshot(std::istream &stream);
void write_snapshot(const SimulationSnapshot &snapshot, std::ostream &stream);
} // namespace pds
