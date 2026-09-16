#pragma once
#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace pds {
// Append-only display index. Original samples stay intact for export and analysis.
class ExtremaIndex {
    static constexpr size_t block_size = 64;
    size_t count_ = 0;
    std::vector<std::pair<size_t, size_t>> blocks_;

  public:
    template <class Value> void append(size_t count, Value value) {
        if (count < count_) {
            count_ = 0;
            blocks_.clear();
        }
        for (; count_ < count; ++count_) {
            if (count_ % block_size == 0)
                blocks_.emplace_back(count_, count_);
            auto &[low, high] = blocks_.back();
            const auto current = value(count_);
            if (current < value(low))
                low = count_;
            if (current > value(high))
                high = count_;
        }
    }
    // Nonempty half-open sample range. Equal extrema retain their earliest index.
    template <class Value> std::pair<size_t, size_t> range(size_t from, size_t to, Value value) const {
        size_t low = from, high = from;
        auto consider = [&](size_t a, size_t b) {
            if (value(a) < value(low))
                low = a;
            if (value(b) > value(high))
                high = b;
        };
        while (from < to && from % block_size) {
            consider(from, from);
            ++from;
        }
        while (to - from >= block_size) {
            const auto [a, b] = blocks_[from / block_size];
            consider(a, b);
            from += block_size;
        }
        while (from < to) {
            consider(from, from);
            ++from;
        }
        return {low, high};
    }
};
} // namespace pds
