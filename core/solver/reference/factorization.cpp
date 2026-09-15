#include "core/solver/reference/factorization.hpp"
#include <algorithm>
#include <cmath>
namespace pds {
std::vector<double> FactorizationCache::solve(const StampSystem &system, const SimulationIR &ir,
                                              double time) {
    return solve_reusing(system, ir, time, false);
}
const std::vector<double> &FactorizationCache::solve_fixed(const StampSystem &system, const SimulationIR &ir,
                                                           double time) {
    return solve_reusing(system, ir, time, true);
}
const std::vector<double> &FactorizationCache::solve_reusing(const StampSystem &system,
                                                             const SimulationIR &ir, double time,
                                                             bool fixed) {
    const size_t n = system.rhs.size();
    if (n > 64)
        return values_ = system.solve(ir, time);
    if (!fixed || entries_.empty()) {
        matrix_.assign(n * n, 0);
        for (size_t r = 0; r < n; ++r)
            for (auto [c, v] : system.rows[r])
                matrix_[r * n + c] = v;
    }
    size_t found = entries_.size();
    if (recent_ < entries_.size() && (fixed || entries_[recent_].matrix == matrix_))
        found = recent_;
    else
        for (size_t i = 0; i < entries_.size(); ++i)
            if (entries_[i].matrix == matrix_) {
                found = i;
                break;
            }
    if (found == entries_.size()) {
        Entry entry;
        entry.matrix = matrix_;
        entry.upper = matrix_;
        entry.factors.resize(n * n);
        entry.scales.resize(n, 1);
        entry.pivots.resize(n);
        auto &a = entry.upper;
        for (size_t r = 0; r < n; ++r) {
            double scale = 0;
            for (size_t c = 0; c < n; ++c) {
                if (!std::isfinite(a[r * n + c]))
                    throw Diagnostic("nonfinite_stamp", ir.unknowns[r].object,
                                     "Nonfinite equation coefficient", time);
                scale = std::max(scale, std::abs(a[r * n + c]));
            }
            if (scale > 0) {
                entry.scales[r] = scale;
                for (size_t c = 0; c < n; ++c)
                    a[r * n + c] /= scale;
            }
        }
        for (size_t k = 0; k < n; ++k) {
            size_t pivot = k;
            for (size_t r = k + 1; r < n; ++r)
                if (std::abs(a[r * n + k]) > std::abs(a[pivot * n + k]))
                    pivot = r;
            if (std::abs(a[pivot * n + k]) < 1e-14)
                throw Diagnostic("singular_matrix", ir.unknowns[k].object,
                                 "Singular or ill-conditioned equations: check floating islands, ideal loops "
                                 "and initial conditions; no stabilizer was added",
                                 time);
            entry.pivots[k] = pivot;
            for (size_t c = k; c < n; ++c)
                std::swap(a[k * n + c], a[pivot * n + c]);
            for (size_t r = k + 1; r < n; ++r) {
                const double f = a[r * n + k] / a[k * n + k];
                entry.factors[k * n + r] = f;
                if (f == 0)
                    continue;
                a[r * n + k] = 0;
                for (size_t c = k + 1; c < n; ++c)
                    a[r * n + c] -= f * a[k * n + c];
            }
        }
        // Store only arithmetic that can change the RHS/solution. This keeps
        // the elimination order while avoiding scans of structural zeros.
        for (size_t k = 0; k < n; ++k) {
            entry.lower_offsets.push_back(entry.lower_terms.size());
            for (size_t r = k + 1; r < n; ++r)
                if (entry.factors[k * n + r] != 0)
                    entry.lower_terms.push_back({r, entry.factors[k * n + r]});
            entry.upper_offsets.push_back(entry.upper_terms.size());
            for (size_t c = k + 1; c < n; ++c)
                if (entry.upper[k * n + c] != 0)
                    entry.upper_terms.push_back({c, entry.upper[k * n + c]});
        }
        entry.lower_offsets.push_back(entry.lower_terms.size());
        entry.upper_offsets.push_back(entry.upper_terms.size());
        if (entries_.size() < 16) {
            found = entries_.size();
            entries_.push_back(std::move(entry));
        } else {
            found = replace_;
            replace_ = (replace_ + 1) % 16;
            entries_[found] = std::move(entry);
        }
    }
    recent_ = found;
    const auto &entry = entries_[found];
    rhs_ = system.rhs;
    auto &b = rhs_;
    for (size_t r = 0; r < n; ++r) {
        if (!std::isfinite(b[r]))
            throw Diagnostic("nonfinite_stamp", ir.unknowns[r].object, "Nonfinite equation right hand side",
                             time);
        b[r] /= entry.scales[r];
    }
    for (size_t k = 0; k < n; ++k) {
        if (k != entry.pivots[k])
            std::swap(b[k], b[entry.pivots[k]]);
        for (size_t i = entry.lower_offsets[k]; i < entry.lower_offsets[k + 1]; ++i) {
            const auto &term = entry.lower_terms[i];
            b[term.index] -= term.value * b[k];
        }
    }
    values_.resize(n);
    auto &x = values_;
    for (size_t r = n; r-- > 0;) {
        double v = b[r];
        for (size_t i = entry.upper_offsets[r]; i < entry.upper_offsets[r + 1]; ++i) {
            const auto &term = entry.upper_terms[i];
            v -= term.value * x[term.index];
        }
        x[r] = v / entry.upper[r * n + r];
        if (!std::isfinite(x[r]))
            throw Diagnostic("nonfinite_solution", ir.unknowns[r].object, "Solution is nonfinite", time);
    }
    return x;
}
} // namespace pds
