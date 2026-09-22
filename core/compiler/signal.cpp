#include "core/compiler/signal.hpp"
#include "core/model/connectivity.hpp"
#include "core/model/hierarchy.hpp"

#include <algorithm>

namespace pds {
namespace {
SignalPortIR signal_port(const CodePort &port) {
    return {port.id, port.name, port.unit, port.type, port.initial};
}

void validate_source(const Project &project, const Endpoint &source, const CodeBlock &block,
                     const CodePort &input) {
    SignalScalarType type = SignalScalarType::real;
    std::string unit;
    bool found = false;
    for (const auto &producer : project.code_blocks)
        if (producer.id == source.object)
            for (const auto &output : producer.outputs)
                if (output.id == source.port) {
                    type = output.type;
                    unit = output.unit;
                    found = true;
                }
    for (const auto &component : project.components)
        if (component.id == source.object && source.port == "out") {
            if (component.kind == Kind::voltage_probe || component.kind == Kind::current_probe) {
                unit = component.kind == Kind::voltage_probe ? "V" : "A";
                found = true;
            }
        }
    for (const auto &pattern : project.patterns)
        if (pattern.id == source.object && source.port.rfind("out", 0) == 0) {
            type = SignalScalarType::boolean;
            found = true;
        }
    if (!found)
        throw Diagnostic("missing_signal_source", block.id,
                         "Input '" + input.name + "' source is not a signal or Gate output");
    if (type != input.type || unit != input.unit)
        throw Diagnostic("incompatible_signal_value", block.id,
                         "Input '" + input.name + "' type or unit differs from its source");
}
} // namespace

SignalIR compile_signal_ir(const Project &source) {
    auto expanded = flatten(source);
    try {
        const auto &project = expanded.project;
        const auto resolved = resolve_connections(project, expanded.origins);
        SignalIR ir;
        for (const auto &block : project.code_blocks) {
            SignalTaskIR task;
            task.id = block.id;
            task.period = block.period;
            task.phase = block.phase;
            task.code = block.code;
            for (const auto &input : block.inputs) {
                const auto driver = resolved.signal_drivers.find(endpoint_key({block.id, input.id}));
                if (driver == resolved.signal_drivers.end())
                    throw Diagnostic("missing_signal_source", block.id,
                                     "Input '" + input.name + "' is not connected");
                validate_source(project, driver->second, block, input);
                task.inputs.push_back({signal_port(input), {driver->second.object, driver->second.port}});
            }
            for (const auto &output : block.outputs)
                task.outputs.push_back(signal_port(output));
            ir.tasks.push_back(std::move(task));
        }
        std::sort(ir.tasks.begin(), ir.tasks.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
        validate_signal_ir(ir);
        return ir;
    } catch (Diagnostic &error) {
        if (const auto origin = expanded.origins.find(error.object); origin != expanded.origins.end()) {
            error.object = origin->second.object;
            error.path = origin->second.instances;
        }
        throw;
    }
}

} // namespace pds
