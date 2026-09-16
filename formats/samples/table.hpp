#pragma once
#include "core/model/model.hpp"
#include <istream>
namespace pds {
// Two-column time/value text; data is returned by value and embedded in the project.
std::vector<Point> read_sample_table(std::istream &input, const std::string &value_unit);
inline constexpr size_t sample_table_max_bytes = 16 * 1024 * 1024;
inline constexpr size_t sample_table_max_rows = 100000;
} // namespace pds
