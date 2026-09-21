#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace pds {

// Select samples for decorative point markers. The recorded history remains
// untouched; when more samples are visible than the raster can distinguish,
// return a deterministic, uniformly distributed subset including both ends.
inline std::vector<size_t> display_sample_indices(size_t from, size_t to, size_t budget) {
    std::vector<size_t> result;
    if (from >= to || budget == 0)
        return result;
    const size_t count = to - from;
    budget = std::min(budget, count);
    result.reserve(budget);
    if (budget == 1) {
        result.push_back(from);
        return result;
    }
    const size_t intervals = budget - 1;
    const size_t quotient = (count - 1) / intervals;
    const size_t remainder = (count - 1) % intervals;
    size_t current = from, carry = 0;
    for (size_t index = 0; index < budget; ++index) {
        result.push_back(current);
        if (index + 1 == budget)
            break;
        current += quotient;
        // Bresenham-style remainder distribution avoids multiplying two
        // potentially large size_t values.
        if (remainder && carry >= intervals - remainder) {
            carry -= intervals - remainder;
            ++current;
        } else {
            carry += remainder;
        }
    }
    return result;
}

} // namespace pds
