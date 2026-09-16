#pragma once
#include "core/experiment/sweep.hpp"
#include <iosfwd>
namespace pds {
void write_experiment_csv_header(std::ostream &);
void write_experiment_csv_case(std::ostream &, const ExperimentCase &);
} // namespace pds
