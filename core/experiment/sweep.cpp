#include "core/experiment/sweep.hpp"
#include "core/editor/properties.hpp"
#include "core/ir/ir.hpp"
#include "core/model/hierarchy.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>

namespace pds {
namespace {
constexpr size_t maximum_cases = 100000;
double &profile_parameter(Profile &p, const std::string &field) {
    if (field == "stop")
        return p.stop;
    if (field == "step")
        return p.step;
    if (field == "warmup")
        return p.warmup;
    if (field == "relative_tolerance")
        return p.relative_tolerance;
    if (field == "voltage_tolerance")
        return p.voltage_tolerance;
    if (field == "current_tolerance")
        return p.current_tolerance;
    if (field == "minimum_step")
        return p.step_control.minimum_step;
    if (field == "adaptive_relative_tolerance")
        return p.step_control.relative_tolerance;
    throw Diagnostic("unknown_property", "", "Unsupported profile parameter: " + field);
}
void apply(Project &p, const ParameterOverride &v) {
    if (!std::isfinite(v.value))
        throw Diagnostic("invalid_sweep_value", v.target.object, "Sweep values must be finite");
    if (v.target.object.empty()) {
        profile_parameter(p.profile, v.target.field) = v.value;
        return;
    }
    if (v.target.field == "x" || v.target.field == "y")
        throw Diagnostic("invalid_sweep_target", v.target.object, "Geometry is not a simulation parameter");
    const auto type = object_type(p, v.target.object);
    if (type == "plot" || type == "node" || type == "ground" || type == "wire")
        throw Diagnostic("invalid_sweep_target", v.target.object,
                         "Choose a component or gate source parameter");
    const auto previous = read_property(p, v.target.object, v.target.field);
    PropertyValue next;
    if (std::holds_alternative<double>(previous))
        next = v.value;
    else if (std::holds_alternative<bool>(previous) && (v.value == 0 || v.value == 1))
        next = v.value != 0;
    else if (std::holds_alternative<unsigned>(previous) && v.value >= 0 &&
             v.value <= std::numeric_limits<unsigned>::max() && v.value == std::floor(v.value))
        next = unsigned(v.value);
    else
        throw Diagnostic("invalid_sweep_target", v.target.object,
                         "Sweep requires a scalar parameter of the correct type");
    write_property(p, v.target.object, v.target.field, next);
}
} // namespace
std::vector<double> sweep_values(double first, double last, size_t count, bool logarithmic) {
    if (!count || count > maximum_cases || !std::isfinite(first) || !std::isfinite(last) ||
        (logarithmic && (first <= 0 || last <= 0)))
        throw Diagnostic("invalid_sweep_range", "", "Invalid linear/logarithmic sweep range");
    std::vector<double> values(count, first);
    for (size_t i = 1; i < count; ++i) {
        const double x = double(i) / double(count - 1);
        values[i] =
            logarithmic ? std::exp(std::lerp(std::log(first), std::log(last), x)) : std::lerp(first, last, x);
    }
    if (count > 1)
        values.back() = last;
    return values;
}
ParameterTarget resolve_parameter(const Project &p, const ParameterTarget &target) {
    if (target.object.empty()) {
        if (!target.instances.empty())
            throw Diagnostic("invalid_sweep_target", "", "Profile has no instance path");
        Profile profile;
        (void)profile_parameter(profile, target.field);
        return target;
    }
    const Schematic *body = &p;
    for (const auto &id : target.instances) {
        auto instance = std::find_if(body->instances.begin(), body->instances.end(),
                                     [&](const auto &i) { return i.id == id; });
        if (instance == body->instances.end())
            throw Diagnostic("missing_instance", id, "Sweep instance does not exist");
        body = &definition(p, instance->definition);
    }
    auto path = target.instances;
    auto object = target.object, field = target.field;
    for (;;) {
        auto instance = std::find_if(body->instances.begin(), body->instances.end(),
                                     [&](const auto &i) { return i.id == object; });
        if (instance == body->instances.end())
            break;
        if (path.size() >= 64 || field.rfind("parameter/", 0) != 0)
            throw Diagnostic("invalid_sweep_target", object, "Choose a public numeric instance parameter");
        const auto &d = definition(p, instance->definition);
        auto parameter = std::find_if(d.parameters.begin(), d.parameters.end(),
                                      [&](const auto &v) { return v.id == field.substr(10); });
        if (parameter == d.parameters.end())
            throw Diagnostic("unknown_property", object, "Unknown public parameter");
        path.push_back(object);
        object = parameter->object;
        field = parameter->field;
        body = &d;
        if (std::any_of(body->instances.begin(), body->instances.end(),
                        [&](const auto &i) { return i.id == object; }))
            field = "parameter/" + field;
    }
    return {{}, expanded_uuid(path, object), field};
}
Project experiment_project(const Project &source, const std::vector<ParameterOverride> &overrides) {
    auto flat = flatten(source).project;
    flat.experiments.clear();
    std::set<std::pair<std::string, std::string>> targets;
    for (const auto &v : overrides) {
        auto target = resolve_parameter(source, v.target);
        if (!targets.emplace(target.object, target.field).second)
            throw Diagnostic("duplicate_sweep_target", target.object,
                             "Two overrides address the same atomic parameter");
        apply(flat, {target, v.value});
    }
    return flat;
}
ExperimentProgress run_experiment(const Project &source, const Experiment &e, const std::atomic_bool *cancel,
                                  const std::function<void(ExperimentCase &&)> &completed,
                                  std::atomic<double> *simulated_time) {
    ExperimentProgress progress;
    progress.total = experiment_size(e);
    const size_t scenarios = std::max<size_t>(1, e.scenarios.size());
    for (size_t index = 0; index < progress.total; ++index) {
        if (cancel && cancel->load()) {
            progress.cancelled = true;
            break;
        }
        ExperimentCase result;
        result.index = index;
        const auto start = std::chrono::steady_clock::now();
        size_t combination = index / scenarios;
        if (!e.scenarios.empty()) {
            const auto &scenario = e.scenarios[index % scenarios];
            result.scenario = scenario.name;
            result.parameters = scenario.overrides;
        }
        std::vector<ParameterOverride> axis_values(e.axes.size());
        for (size_t i = e.axes.size(); i-- > 0;) {
            const auto &axis = e.axes[i];
            axis_values[i] = {axis.target, axis.values[combination % axis.values.size()]};
            combination /= axis.values.size();
        }
        result.parameters.insert(result.parameters.end(), axis_values.begin(), axis_values.end());
        try {
            auto project = experiment_project(source, result.parameters);
            const auto ir = compile(project);
            const Recording recording{false, e.channels};
            auto run = execute(ir, cancel, simulated_time, &recording);
            result.cancelled = run.cancelled;
            result.accepted_steps = run.accepted_steps;
            result.simulated_time = run.last_time;
            const double end = e.end < 0 ? run.last_time : e.end;
            for (int ch = 0; ch < int(run.channels.size() + run.gate_objects.size()); ++ch) {
                ExperimentMeasurement m;
                m.channel = result_channel(run, ch);
                m.time = time_statistics(run, ch, e.begin, end);
                auto first = std::lower_bound(run.samples.begin(), run.samples.end(), e.begin,
                                              [](const Sample &s, double t) { return s.time < t; });
                for (auto it = first; it != run.samples.end() && it->time <= end; ++it) {
                    const double value = channel_value(run, size_t(it - run.samples.begin()), ch);
                    if (!m.has_samples)
                        m.minimum = m.maximum = value;
                    m.minimum = std::min(m.minimum, value);
                    m.maximum = std::max(m.maximum, value);
                    m.final_value = value;
                    m.has_samples = true;
                }
                result.measurements.push_back(std::move(m));
            }
            if (e.retain_curves)
                result.curves = std::move(run);
        } catch (const Diagnostic &error) {
            result.error_code = error.code;
            result.error_object = error.object;
            result.error_message = error.what();
        }
        result.elapsed_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (!result.error_code.empty())
            ++progress.failed;
        if (result.cancelled)
            progress.cancelled = true;
        else
            ++progress.completed;
        if (completed)
            completed(std::move(result));
        if (progress.cancelled)
            break;
    }
    return progress;
}
} // namespace pds
