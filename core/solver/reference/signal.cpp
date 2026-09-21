#include "core/solver/reference/signal.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pds {
namespace {
double scheduled_time(const SignalTaskIR &task, std::uint64_t tick) {
    return task.phase + static_cast<double>(tick) * task.period;
}

CProgram compile_task(const SignalTaskIR &task) {
    CProgramOptions options;
    options.diagnostic_code = "invalid_code_block";
    options.object = task.id;
    options.allow_time = true;
    options.external_variables.insert("dt");
    for (const auto &input : task.inputs)
        options.external_variables.insert(input.port.name);
    for (const auto &output : task.outputs) {
        options.external_variables.insert(output.name);
        options.writable_variables.insert(output.name);
    }
    return compile_c_program(task.code, options);
}

SignalValue checked_value(const SignalInputIR &input, const SignalFrame &frame, const SignalFrame &prior_outputs,
                          double time, const std::string &task) {
    const auto key = signal_endpoint_key(input.source);
    const SignalValue *value = nullptr;
    if (const auto found = frame.find(key); found != frame.end())
        value = &found->second;
    else if (const auto previous = prior_outputs.find(key); previous != prior_outputs.end())
        value = &previous->second;
    if (!value)
        return {input.port.type, input.port.unit, input.port.initial, time, false};
    if (!value->valid)
        return {input.port.type, input.port.unit, input.port.initial, time, false};
    if (value->type != input.port.type || value->unit != input.port.unit)
        throw Diagnostic("incompatible_signal_value", task,
                         "Signal input '" + input.port.name + "' has an incompatible type or unit", time);
    const double tolerance = std::max(1.0, std::abs(time)) * 1e-13;
    if (!std::isfinite(value->value) || value->time > time + tolerance)
        throw Diagnostic("invalid_signal_frame", task,
                         "Signal input '" + input.port.name + "' has an invalid frame", time);
    return *value;
}
} // namespace

SignalRuntimeState initialize_signal_runtime(const SignalIR &ir) {
    validate_signal_ir(ir);
    SignalRuntimeState state;
    for (const auto &task : ir.tasks) {
        auto &runtime = state.tasks[task.id];
        runtime.program = compile_task(task);
        for (const auto &output : task.outputs) {
            runtime.outputs[output.name] = output.initial;
            state.outputs[signal_endpoint_key({task.id, output.id})] =
                {output.type, output.unit, output.initial, 0, false};
        }
    }
    return state;
}

std::vector<SignalEmission> run_signal_tasks(const SignalIR &ir, SignalRuntimeState &state,
                                             double through_time, const SignalFrame &accepted_inputs) {
    if (!std::isfinite(through_time) || through_time < 0)
        throw Diagnostic("invalid_signal_time", "", "Signal scheduler time must be finite and non-negative");
    std::vector<SignalEmission> emissions;
    std::size_t events = 0;
    for (;;) {
        double next = std::numeric_limits<double>::infinity();
        for (const auto &task : ir.tasks) {
            const auto runtime = state.tasks.find(task.id);
            if (runtime == state.tasks.end())
                throw Diagnostic("invalid_signal_state", task.id, "Signal runtime state does not match its IR");
            next = std::min(next, scheduled_time(task, runtime->second.next_tick));
        }
        const double tolerance = std::max(1.0, std::abs(through_time)) * 1e-13;
        if (next > through_time + tolerance)
            break;
        if (++events > 1000000)
            throw Diagnostic("signal_event_budget", "", "Signal scheduler event budget exceeded", next);
        const auto prior_outputs = state.outputs;
        std::vector<const SignalTaskIR *> due;
        for (const auto &task : ir.tasks)
            if (std::abs(scheduled_time(task, state.tasks.at(task.id).next_tick) - next) <= tolerance)
                due.push_back(&task);
        std::sort(due.begin(), due.end(), [](const auto *a, const auto *b) { return a->id < b->id; });
        std::vector<SignalEmission> pending;
        for (const auto *task : due) {
            auto &runtime = state.tasks.at(task->id);
            std::map<std::string, double> values{{"dt", task->period}};
            for (const auto &input : task->inputs)
                values[input.port.name] = checked_value(input, accepted_inputs, prior_outputs, next, task->id).value;
            for (const auto &output : task->outputs)
                values[output.name] = runtime.outputs.at(output.name);
            try {
                const auto result = execute_c_program(runtime.program, next, &runtime.program_state, values);
                for (const auto &output : task->outputs) {
                    auto value = result.variables.at(output.name);
                    if (!std::isfinite(value))
                        throw Diagnostic("invalid_signal_output", task->id,
                                         "Code-block output '" + output.name + "' is not finite", next);
                    if (output.type == SignalScalarType::boolean)
                        value = value == 0 ? 0 : 1;
                    runtime.outputs[output.name] = value;
                    pending.push_back({{task->id, output.id},
                                       {output.type, output.unit, value, next, true}});
                }
            } catch (Diagnostic &error) {
                error.time = next;
                throw;
            }
            ++runtime.next_tick;
        }
        for (const auto &emission : pending) {
            state.outputs[signal_endpoint_key(emission.endpoint)] = emission.value;
            emissions.push_back(emission);
        }
    }
    return emissions;
}

} // namespace pds
