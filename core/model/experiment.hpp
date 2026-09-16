#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace pds {
struct ParameterTarget {
    std::vector<std::string> instances;
    std::string object, field;
    // Empty object with empty path addresses a numeric profile field.
    bool operator==(const ParameterTarget &) const = default;
};
struct ParameterOverride {
    ParameterTarget target;
    double value = 0;
    bool operator==(const ParameterOverride &) const = default;
};
struct SweepAxis {
    ParameterTarget target;
    std::vector<double> values;
    bool operator==(const SweepAxis &) const = default;
};
struct Scenario {
    std::string name;
    std::vector<ParameterOverride> overrides;
    bool operator==(const Scenario &) const = default;
};
struct Experiment {
    std::string id, name;
    std::vector<SweepAxis> axes;
    std::vector<Scenario> scenarios;
    std::vector<std::string> channels;
    double begin = 0, end = -1;
    bool retain_curves = false;
    bool operator==(const Experiment &) const = default;
};
size_t experiment_size(const Experiment &);
} // namespace pds
