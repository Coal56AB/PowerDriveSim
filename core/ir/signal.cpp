#include "core/ir/signal.hpp"
#include "core/model/c_program.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

namespace pds {
namespace {
bool identifier(const std::string &name) {
    if (name.empty() || (!std::isalpha(static_cast<unsigned char>(name[0])) && name[0] != '_'))
        return false;
    return std::all_of(name.begin() + 1, name.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_';
    });
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
} // namespace

std::string signal_endpoint_key(const SignalEndpointIR &endpoint) {
    return endpoint.object + "/" + endpoint.port;
}

void validate_signal_ir(const SignalIR &ir) {
    std::set<std::string> task_ids;
    for (const auto &task : ir.tasks) {
        if (!valid_uuid(task.id) || !task_ids.insert(task.id).second)
            throw Diagnostic("invalid_signal_task", task.id, "Signal task UUID must be unique");
        if (!std::isfinite(task.period) || task.period <= 0 || !std::isfinite(task.phase) || task.phase < 0)
            throw Diagnostic("invalid_signal_task", task.id, "Signal task period must be positive and phase non-negative");
        if (task.code.empty())
            throw Diagnostic("invalid_code_block", task.id, "Code block source must not be empty");
        if (task.outputs.empty())
            throw Diagnostic("invalid_code_block", task.id, "Code block requires at least one output");
        std::set<std::string> port_ids, names{"t", "dt"};
        auto port = [&](const SignalPortIR &candidate) {
            if (!valid_uuid(candidate.id) || !port_ids.insert(candidate.id).second)
                throw Diagnostic("invalid_signal_port", task.id, "Signal port UUID must be unique within its block");
            if (!identifier(candidate.name) || !names.insert(candidate.name).second)
                throw Diagnostic("invalid_signal_port", candidate.id, "Signal port name must be a unique C identifier");
            if (!std::isfinite(candidate.initial))
                throw Diagnostic("invalid_signal_port", candidate.id, "Signal port initial value must be finite");
            if (candidate.type == SignalScalarType::boolean && candidate.initial != 0 && candidate.initial != 1)
                throw Diagnostic("invalid_signal_port", candidate.id, "Boolean initial value must be 0 or 1");
        };
        for (const auto &input : task.inputs) {
            port(input.port);
            if (input.source.object.empty() || input.source.port.empty())
                throw Diagnostic("missing_signal_source", input.port.id, "Signal input source must be connected");
        }
        for (const auto &output : task.outputs)
            port(output);
        (void)compile_task(task);
    }
    for (const auto &task : ir.tasks)
        for (const auto &input : task.inputs) {
            const auto source_task = std::find_if(ir.tasks.begin(), ir.tasks.end(), [&](const SignalTaskIR &candidate) {
                return candidate.id == input.source.object;
            });
            if (source_task == ir.tasks.end())
                continue; // Probe/sensor source is typed by the electrical compiler.
            const auto output = std::find_if(source_task->outputs.begin(), source_task->outputs.end(),
                                             [&](const SignalPortIR &candidate) {
                                                 return candidate.id == input.source.port;
                                             });
            if (output == source_task->outputs.end())
                throw Diagnostic("missing_signal_source", input.port.id, "Connected code-block output does not exist");
            if (output->type != input.port.type || output->unit != input.port.unit)
                throw Diagnostic("incompatible_signal_value", input.port.id,
                                 "Connected code-block ports have incompatible types or units");
        }
}

} // namespace pds
