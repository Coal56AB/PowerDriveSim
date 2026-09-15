#pragma once
#include "core/solver/reference/reference.hpp"
namespace pds {
// Bounded, exact-coefficient cache for small systems. Larger systems retain the sparse path.
class FactorizationCache {
  public:
    std::vector<double> solve(const StampSystem &system, const SimulationIR &ir, double time);
    // The caller owns an immutable matrix; only rhs changes between calls.
    const std::vector<double> &solve_fixed(const StampSystem &system, const SimulationIR &ir, double time);

  private:
    struct Entry {
        struct Term {
            size_t index;
            double value;
        };
        std::vector<double> matrix, upper, factors, scales;
        std::vector<size_t> pivots;
        std::vector<size_t> lower_offsets, upper_offsets;
        std::vector<Term> lower_terms, upper_terms;
    };
    std::vector<Entry> entries_;
    std::vector<double> matrix_;
    std::vector<double> rhs_, values_;
    const std::vector<double> &solve_reusing(const StampSystem &, const SimulationIR &, double, bool fixed);
    size_t recent_ = 0, replace_ = 0;
};
} // namespace pds
