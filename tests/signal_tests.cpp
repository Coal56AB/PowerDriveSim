#include "core/solver/reference/signal.hpp"
#include "core/solver/reference/reference.hpp"
#include "core/model/hierarchy.hpp"
#include "core/model/connectivity.hpp"
#include "core/compiler/signal.hpp"
#include "core/ir/ir.hpp"
#include "formats/project/project.hpp"
#include "formats/snapshot/snapshot.hpp"

#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>

using namespace pds;

namespace {
void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
void near(double actual, double expected, double tolerance, const char *message) {
    if (std::abs(actual - expected) > tolerance)
        throw std::runtime_error(message);
}
void error(const std::string &code, const std::function<void()> &operation) {
    try {
        operation();
    } catch (const Diagnostic &diagnostic) {
        if (diagnostic.code == code)
            return;
        throw;
    }
    throw std::runtime_error("Expected diagnostic " + code);
}
SignalPortIR port(const std::string &key, const std::string &name, const std::string &unit,
                  SignalScalarType type = SignalScalarType::real, double initial = 0) {
    return {derived_uuid(key), name, unit, type, initial};
}
} // namespace

int main() {
    try {
        const auto sensor = derived_uuid("signal-sensor");
        SignalTaskIR pi;
        pi.id = derived_uuid("signal-pi");
        pi.period = .1;
        pi.code = "static double integral = 0; integral += error * dt; command = 2 * error + integral;";
        pi.inputs = {{port("signal-pi-error", "error", "V"), {sensor, "out"}}};
        pi.outputs = {port("signal-pi-command", "command", "V")};
        SignalIR ir{{pi}};
        auto state = initialize_signal_runtime(ir);
        SignalFrame frame{{signal_endpoint_key({sensor, "out"}),
                           {SignalScalarType::real, "V", 3, 0, true}}};
        auto emitted = run_signal_tasks(ir, state, 0, frame);
        check(emitted.size() == 1, "PI task executes at phase zero");
        near(emitted[0].value.value, 6.3, 1e-15, "PI first output");
        frame.begin()->second = {SignalScalarType::real, "V", 2, .1, true};
        emitted = run_signal_tasks(ir, state, .1, frame);
        near(emitted[0].value.value, 4.5, 1e-15, "PI state persists between deterministic ticks");

        SignalTaskIR producer;
        producer.id = derived_uuid("signal-producer");
        producer.period = .1;
        producer.code = "out = source;";
        producer.inputs = {{port("signal-producer-in", "source", "A"), {sensor, "current"}}};
        producer.outputs = {port("signal-producer-out", "out", "A")};
        SignalTaskIR consumer;
        consumer.id = derived_uuid("signal-consumer");
        consumer.period = .1;
        consumer.code = "scaled = input * 10;";
        consumer.inputs = {{port("signal-consumer-in", "input", "A", SignalScalarType::real, 1),
                            {producer.id, producer.outputs[0].id}}};
        consumer.outputs = {port("signal-consumer-out", "scaled", "A")};
        SignalIR chain{{consumer, producer}}; // Reverse order must not create same-timestamp feedthrough.
        auto chain_state = initialize_signal_runtime(chain);
        SignalFrame current{{signal_endpoint_key({sensor, "current"}),
                             {SignalScalarType::real, "A", 2, 0, true}}};
        auto chain_emissions = run_signal_tasks(chain, chain_state, 0, current);
        auto output = [&](const std::string &task) {
            const auto found = std::find_if(chain_emissions.begin(), chain_emissions.end(),
                                            [&](const SignalEmission &entry) {
                                                return entry.endpoint.object == task;
                                            });
            if (found == chain_emissions.end())
                throw std::runtime_error("Missing signal emission");
            return found->value.value;
        };
        near(output(producer.id), 2, 0, "Producer output");
        near(output(consumer.id), 10, 0, "Consumer uses its initial input at the same timestamp");
        chain_emissions = run_signal_tasks(chain, chain_state, .1, current);
        near(output(consumer.id), 20, 0, "Consumer reads the last committed producer frame");

        auto invalid = ir;
        invalid.tasks[0].code = "error = 1;";
        error("invalid_code_block", [&] { (void)initialize_signal_runtime(invalid); });
        auto mismatch_state = initialize_signal_runtime(ir);
        SignalFrame mismatch{{signal_endpoint_key({sensor, "out"}),
                              {SignalScalarType::boolean, "V", 1, 0, true}}};
        error("incompatible_signal_value", [&] { (void)run_signal_tasks(ir, mismatch_state, 0, mismatch); });
        auto future_state = initialize_signal_runtime(ir);
        SignalFrame future{{signal_endpoint_key({sensor, "out"}),
                            {SignalScalarType::real, "V", 1, .2, true}}};
        error("invalid_signal_frame", [&] { (void)run_signal_tasks(ir, future_state, 0, future); });

        Project nested;
        nested.id = derived_uuid("code-hierarchy-root");
        nested.wired = true;
        Definition body;
        body.id = derived_uuid("code-hierarchy-definition");
        body.wired = true;
        CodeBlock block;
        block.id = derived_uuid("code-hierarchy-block");
        block.name = "Logic";
        block.x = 10;
        block.code = "out = 1;";
        block.outputs = {{derived_uuid("code-hierarchy-output"), "out", "", SignalScalarType::real, 0}};
        body.code_blocks.push_back(block);
        const auto public_output = derived_uuid("code-hierarchy-public-output");
        body.ports.push_back({public_output, "out", {block.id, block.outputs[0].id},
                              Domain::signal, Direction::output});
        nested.definitions.push_back(body);
        const auto first = derived_uuid("code-hierarchy-first");
        const auto second = derived_uuid("code-hierarchy-second");
        nested.instances = {{first, "First", body.id, 100, 0}, {second, "Second", body.id, 200, 0}};
        const auto flattened = flatten(nested);
        check(flattened.project.code_blocks.size() == 2, "Code blocks survive hierarchy expansion");
        check(flattened.project.code_blocks[0].id == expanded_uuid({first}, block.id) &&
                  flattened.project.code_blocks[1].id == expanded_uuid({second}, block.id),
              "Code blocks have independent instance identities");
        check(flattened.project.code_blocks[0].x == 110 && flattened.project.code_blocks[1].x == 210 &&
                  flattened.origins.at(flattened.project.code_blocks[1].id).instances ==
                      std::vector<std::string>{second},
              "Code block geometry and origin follow the instance");
        check(flattened.terminals.at(endpoint_key({second, public_output})) ==
                  Endpoint{expanded_uuid({second}, block.id), block.outputs[0].id},
              "Public code-block port resolves to the expanded instance");
        CodeBlock nested_consumer;
        nested_consumer.id = derived_uuid("code-hierarchy-consumer");
        nested_consumer.code = "echo = input;";
        nested_consumer.inputs = {{derived_uuid("code-hierarchy-consumer-input"), "input", "",
                            SignalScalarType::real, 0}};
        nested_consumer.outputs = {{derived_uuid("code-hierarchy-consumer-output"), "echo", "",
                             SignalScalarType::real, 0}};
        nested.code_blocks.push_back(nested_consumer);
        nested.wires.push_back({derived_uuid("code-hierarchy-link"), {second, public_output},
                                {nested_consumer.id, nested_consumer.inputs[0].id}});
        const auto nested_ir = compile_signal_ir(nested);
        const auto consumer_task = std::find_if(nested_ir.tasks.begin(), nested_ir.tasks.end(),
                                                [&](const auto &task) { return task.id == nested_consumer.id; });
        check(consumer_task != nested_ir.tasks.end() &&
                  consumer_task->inputs[0].source.object == expanded_uuid({second}, block.id),
              "Nested public output drives the root code block through its expanded endpoint");

        Project wired;
        wired.id = derived_uuid("code-wired-project");
        wired.wired = true;
        Component voltage;
        voltage.id = derived_uuid("code-voltage-probe");
        voltage.kind = Kind::voltage_probe;
        Component current_probe;
        current_probe.id = derived_uuid("code-current-probe");
        current_probe.kind = Kind::current_probe;
        wired.components = {voltage, current_probe};
        CodeBlock linked = block;
        linked.inputs = {{derived_uuid("code-voltage-input"), "voltage", "V", SignalScalarType::real, 0},
                         {derived_uuid("code-current-input"), "current", "A", SignalScalarType::real, 0}};
        linked.code = "out = voltage + current;";
        wired.code_blocks.push_back(linked);
        wired.wires = {{derived_uuid("code-voltage-wire"), {voltage.id, "out"}, {linked.id, linked.inputs[0].id}},
                       {derived_uuid("code-current-wire"), {current_probe.id, "out"}, {linked.id, linked.inputs[1].id}}};
        const auto compiled = compile_signal_ir(wired);
        check(compiled.tasks.size() == 1 && compiled.tasks[0].inputs.size() == 2 &&
                  compiled.tasks[0].inputs[0].source.object == voltage.id &&
                  compiled.tasks[0].inputs[1].source.object == current_probe.id,
              "Each code-block input resolves its own typed probe source");
        std::ostringstream serialized;
        write_project(wired, serialized);
        std::istringstream saved(serialized.str());
        check(read_project(saved) == wired, "Wired code-block ports survive project round-trip");
        auto tagged = wired;
        ConnectionTag tag;
        tag.id = derived_uuid("code-signal-tag");
        tag.name = "SENSE";
        tag.domain = Domain::signal;
        tagged.tags.push_back(tag);
        tagged.wires[0].to = {tag.id, "io"};
        tagged.wires.push_back({derived_uuid("code-tag-wire"), {tag.id, "io"},
                                {linked.id, linked.inputs[0].id}});
        check(compile_signal_ir(tagged).tasks[0].inputs[0].source.object == voltage.id,
              "Signal tag resolves the probe driver for a code-block input");
        Project gate_link;
        gate_link.id = derived_uuid("code-gate-link-project");
        gate_link.wired = true;
        GatePattern pattern;
        pattern.id = derived_uuid("code-gate-source");
        gate_link.patterns.push_back(pattern);
        CodeBlock gate_block;
        gate_block.id = derived_uuid("code-gate-block");
        gate_block.code = "gate = control;";
        gate_block.inputs = {{derived_uuid("code-gate-input"), "control", "",
                              SignalScalarType::boolean, 0}};
        gate_block.outputs = {{derived_uuid("code-gate-output"), "gate", "",
                               SignalScalarType::boolean, 0}};
        gate_link.code_blocks.push_back(gate_block);
        gate_link.wires.push_back({derived_uuid("code-gate-wire"), {pattern.id, "out"},
                                   {gate_block.id, gate_block.inputs[0].id}});
        check(compile_signal_ir(gate_link).tasks[0].inputs[0].source.object == pattern.id,
              "Boolean code-block input accepts a Gate source");
        auto duplicate = wired;
        duplicate.wires.push_back({derived_uuid("code-duplicate-wire"),
                                   {current_probe.id, "out"}, {linked.id, linked.inputs[0].id}});
        error("multiple_signal_drivers", [&] { (void)compile_signal_ir(duplicate); });
        auto wrong_unit = wired;
        wrong_unit.code_blocks[0].inputs[0].unit = "A";
        error("incompatible_signal_value", [&] { (void)compile_signal_ir(wrong_unit); });
        auto disconnected = wired;
        disconnected.wires.pop_back();
        error("missing_signal_source", [&] { (void)compile_signal_ir(disconnected); });

        Project integrated;
        integrated.id = derived_uuid("integrated-signal-project");
        integrated.wired = true;
        integrated.profile.step = .1;
        integrated.profile.stop = .31;
        const auto ground = derived_uuid("integrated-ground");
        const auto supply_node = derived_uuid("integrated-supply-node");
        const auto load_node = derived_uuid("integrated-load-node");
        const auto resistor_node = derived_uuid("integrated-resistor-node");
        integrated.nodes = {{ground, "GND", true}, {supply_node, "supply"},
                            {load_node, "load"}, {resistor_node, "resistor"}};
        Component supply;
        supply.id = derived_uuid("integrated-supply");
        supply.name = "V1";
        supply.kind = Kind::voltage;
        supply.value = 1;
        Component sensed;
        sensed.id = derived_uuid("integrated-probe");
        sensed.name = "VP";
        sensed.kind = Kind::voltage_probe;
        Component controlled;
        controlled.id = derived_uuid("integrated-switch");
        controlled.name = "S";
        controlled.kind = Kind::ideal_switch;
        Component measured_current;
        measured_current.id = derived_uuid("integrated-current-probe");
        measured_current.name = "IP";
        measured_current.kind = Kind::current_probe;
        Component load;
        load.id = derived_uuid("integrated-load");
        load.name = "R";
        load.kind = Kind::resistor;
        load.value = 1;
        integrated.components = {supply, sensed, controlled, measured_current, load};
        GatePattern enable;
        enable.id = derived_uuid("integrated-enable");
        enable.name = "Enable";
        enable.initial = true;
        integrated.patterns.push_back(enable);
        CodeBlock controller;
        controller.id = derived_uuid("integrated-controller");
        controller.name = "Controller";
        controller.period = .15;
        controller.code = "static double count = 0; count += 1; gate = count >= 2 && enable && sensed > 0.5 && current > -1;";
        controller.inputs = {{derived_uuid("integrated-controller-input"), "sensed", "V",
                              SignalScalarType::real, 0},
                             {derived_uuid("integrated-current-input"), "current", "A",
                              SignalScalarType::real, 0},
                             {derived_uuid("integrated-enable-input"), "enable", "",
                              SignalScalarType::boolean, 0}};
        controller.outputs = {{derived_uuid("integrated-controller-output"), "gate", "",
                               SignalScalarType::boolean, 0}};
        integrated.code_blocks.push_back(controller);
        auto wire = [&](const std::string& key, Endpoint from, Endpoint to) {
            integrated.wires.push_back({derived_uuid(key),std::move(from),std::move(to)});
        };
        wire("integrated-supply-p",{supply.id,"p"},{supply_node,"node"});
        wire("integrated-supply-n",{supply.id,"n"},{ground,"node"});
        wire("integrated-probe-p",{sensed.id,"p"},{supply_node,"node"});
        wire("integrated-probe-n",{sensed.id,"n"},{ground,"node"});
        wire("integrated-switch-p",{controlled.id,"p"},{supply_node,"node"});
        wire("integrated-switch-n",{controlled.id,"n"},{load_node,"node"});
        wire("integrated-current-p",{measured_current.id,"p"},{load_node,"node"});
        wire("integrated-current-n",{measured_current.id,"n"},{resistor_node,"node"});
        wire("integrated-load-p",{load.id,"p"},{resistor_node,"node"});
        wire("integrated-load-n",{load.id,"n"},{ground,"node"});
        wire("integrated-sense",{sensed.id,"out"},{controller.id,controller.inputs[0].id});
        wire("integrated-current",{measured_current.id,"out"},{controller.id,controller.inputs[1].id});
        wire("integrated-enable-wire",{enable.id,"out"},{controller.id,controller.inputs[2].id});
        wire("integrated-gate",{controller.id,controller.outputs[0].id},{controlled.id,"gate"});
        const auto integrated_ir = compile(integrated);
        check(integrated_ir.signal.tasks.size() == 1 && integrated_ir.signal_inputs.size() == 3 &&
                  integrated_ir.signal_gates.size() == 1,
              "Electrical compilation retains typed voltage, current, Gate and output bindings");
        const auto simulation = execute(integrated_ir);
        const auto load_channel = std::find_if(simulation.channels.begin(),simulation.channels.end(),
                                               [&](const Channel& channel){return channel.object==load_node;});
        const auto gate_channel = std::find(simulation.gate_objects.begin(),simulation.gate_objects.end(),controlled.id);
        check(load_channel != simulation.channels.end() && gate_channel != simulation.gate_objects.end(),
              "Integrated run records the controlled load and Gate");
        const auto load_index = static_cast<std::size_t>(load_channel-simulation.channels.begin());
        const auto gate_index = static_cast<std::size_t>(gate_channel-simulation.gate_objects.begin());
        const auto tick = std::find_if(simulation.samples.begin(),simulation.samples.end(),[](const Sample& sample) {
            return std::abs(sample.time-.15)<1e-15;
        });
        const auto driven = std::find_if(simulation.samples.begin(),simulation.samples.end(),[](const Sample& sample) {
            return std::abs(sample.time-.2)<1e-15;
        });
        check(tick != simulation.samples.end() && tick->gates[gate_index] &&
                  std::abs(tick->values[load_index])<1e-12,
              "Signal tick is exact and cannot change its already accepted electrical frame");
        check(driven != simulation.samples.end() && driven->gates[gate_index] &&
                  std::abs(driven->values[load_index]-1)<1e-12,
              "Code-block Gate output controls the generic switch path on the following interval");
        ExecutionOptions snapshot_options;
        snapshot_options.capture_snapshot = true;
        snapshot_options.max_steps = 2;
        const auto partial_run = execute(integrated_ir,nullptr,nullptr,nullptr,nullptr,{},&snapshot_options);
        check(partial_run.snapshot && partial_run.snapshot->version == 4 &&
                  partial_run.snapshot->signal_tasks.at(controller.id).next_tick == 2 &&
                  !partial_run.snapshot->signal_tasks.at(controller.id).program_state.static_values.empty() &&
                  partial_run.snapshot->accepted_signal_inputs.size() == 3,
              "Code-block snapshot preserves scheduler progress, static state and sensor frame");
        std::ostringstream snapshot_text;
        write_snapshot(*partial_run.snapshot,snapshot_text);
        std::istringstream snapshot_stream(snapshot_text.str());
        const auto restored = read_snapshot(snapshot_stream);
        check(restored == *partial_run.snapshot,"Code-block snapshot survives state-file roundtrip");
        ExecutionOptions continuation;
        continuation.resume = &restored;
        const auto resumed = execute(integrated_ir,nullptr,nullptr,nullptr,nullptr,{},&continuation);
        check(resumed.samples.size() == simulation.samples.size() - partial_run.accepted_steps,
              "Code-block continuation retains the full result timeline");
        for(size_t index=0;index<resumed.samples.size();++index)
            check(resumed.samples[index].time == simulation.samples[partial_run.accepted_steps + index].time &&
                      resumed.samples[index].values == simulation.samples[partial_run.accepted_steps + index].values &&
                      resumed.samples[index].gates == simulation.samples[partial_run.accepted_steps + index].gates,
                  "Code-block continuation is bit-identical across Gate edges");
        auto changed_signal = integrated_ir;
        changed_signal.signal.tasks[0].code = "gate = 0;";
        error("invalid_snapshot", [&] { (void)execute(changed_signal,nullptr,nullptr,nullptr,nullptr,{},&continuation); });
        std::cout << "PASS typed causal Signal IR and deterministic C scheduler\n";
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
