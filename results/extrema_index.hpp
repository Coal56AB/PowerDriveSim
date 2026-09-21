#pragma once
#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace pds {
// Append-only display index. Original samples stay intact for export and analysis.
class ExtremaIndex {
    static constexpr size_t block_size = 64;
    struct Block {
        size_t low, high;
        double low_value, high_value;
    };
    size_t count_ = 0;
    std::vector<Block> blocks_;

  public:
    template <class Value> void append(size_t count, Value value) {
        if (count < count_) {
            count_ = 0;
            blocks_.clear();
        }
        for (; count_ < count; ++count_) {
            const double current = value(count_);
            if (count_ % block_size == 0) {
                blocks_.push_back({count_, count_, current, current});
                continue;
            }
            auto &block = blocks_.back();
            if (current < block.low_value) {
                block.low = count_;
                block.low_value = current;
            }
            if (current > block.high_value) {
                block.high = count_;
                block.high_value = current;
            }
        }
    }
    // Nonempty half-open sample range. Equal extrema retain their earliest index.
    template <class Value> std::pair<size_t, size_t> range(size_t from, size_t to, Value value) const {
        size_t low = from, high = from;
        double low_value = value(from), high_value = low_value;
        auto consider = [&](size_t a, double a_value, size_t b, double b_value) {
            if (a_value < low_value) {
                low = a;
                low_value = a_value;
            }
            if (b_value > high_value) {
                high = b;
                high_value = b_value;
            }
        };
        while (from < to && from % block_size) {
            const double current = value(from);
            consider(from, current, from, current);
            ++from;
        }
        while (to - from >= block_size) {
            const auto &block = blocks_[from / block_size];
            consider(block.low, block.low_value, block.high, block.high_value);
            from += block_size;
        }
        while (from < to) {
            const double current = value(from);
            consider(from, current, from, current);
            ++from;
        }
        return {low, high};
    }
};
} // namespace pds
