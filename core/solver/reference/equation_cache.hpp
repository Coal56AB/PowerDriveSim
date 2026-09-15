#pragma once
#include "core/solver/reference/factorization.hpp"
#include <memory>
namespace pds {
// Lazily prepare only the topologies and exact step lengths actually visited.
// No time-step rounding, topology recognition, or changes to device equations.
class EquationCache {
  public:
    explicit EquationCache(const SimulationIR &ir) : ir_(ir) {}
    const std::vector<double> &solve(double time, double h, bool initialize, const std::vector<bool> &gates,
                                     const std::vector<bool> &diodes, const std::vector<double> &states,
                                     const std::vector<double> &history);
    const StampSystem &system() const { return entries_[recent_]->system; }

  private:
    struct DynamicRhs {
        size_t state;
        int row;
        Kind kind;
        double factor;
    };
    struct Entry {
        double h;
        bool initialize;
        std::vector<bool> gates, diodes;
        StampSystem system;
        FactorizationCache factorization;
        std::vector<double> constant;
        std::vector<DynamicRhs> dynamic;
        explicit Entry(size_t n) : system(n) {}
    };
    const SimulationIR &ir_;
    std::vector<std::unique_ptr<Entry>> entries_;
    size_t recent_ = 0, replace_ = 0;
};
} // namespace pds
