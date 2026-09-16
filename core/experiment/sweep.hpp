#pragma once
#include "core/model/model.hpp"
#include "results/integrals.hpp"
#include <atomic>
#include <functional>

namespace pds {
struct ExperimentMeasurement {
    Channel channel;
    TimeStatistics time;
    double final_value = 0, minimum = 0, maximum = 0;
    bool has_samples = false;
};
struct ExperimentCase {
    size_t index = 0;
    std::string scenario;
    std::vector<ParameterOverride> parameters;
    std::vector<ExperimentMeasurement> measurements;
    std::optional<Result> curves;
    size_t accepted_steps = 0;
    double simulated_time = 0, elapsed_seconds = 0;
    std::string error_code, error_object, error_message;
    bool cancelled = false;
};
struct ExperimentProgress {
    size_t total = 0, completed = 0, failed = 0;
    bool cancelled = false;
};
std::vector<double> sweep_values(double first, double last, size_t count, bool logarithmic = false);
// Resolve a public parameter through nested instances to one atomic property.
ParameterTarget resolve_parameter(const Project &, const ParameterTarget &);
Project experiment_project(const Project &, const std::vector<ParameterOverride> &);
ExperimentProgress run_experiment(const Project &, const Experiment &, const std::atomic_bool *cancel,
                                  const std::function<void(ExperimentCase &&)> &completed,
                                  std::atomic<double> *simulated_time = nullptr);
} // namespace pds
