#pragma once
#include "core/model/model.hpp"
namespace pds {
enum class TimeSide { left, right };
void validate_waveform(const Component &component);
double source_value(const Component &component, double time, TimeSide side = TimeSide::right);
// Next corner/discontinuity strictly after time, or infinity. No precomputed edge array.
double next_source_breakpoint(const Component &component, double time);
double *source_parameter(SourceWaveform &source, const std::string &key);
const double *source_parameter(const SourceWaveform &source, const std::string &key);
} // namespace pds
