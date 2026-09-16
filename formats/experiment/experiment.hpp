#pragma once
#include "core/model/experiment.hpp"
#include <iosfwd>
namespace pds {
// Fields after the project-file "experiment" tag; one line per experiment.
Experiment read_experiment(std::istream &);
void write_experiment(std::ostream &, const Experiment &);
} // namespace pds
