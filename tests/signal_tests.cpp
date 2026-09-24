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

        SignalTaskIR slow;
        slow.id = derived_uuid("signal-multirate-slow");
        slow.period = .2;
        slow.phase = .05;
        slow.code = "out = t;";
        slow.outputs = {port("signal-multirate-slow-out", "out", "s")};
        SignalTaskIR fast;
        fast.id = derived_uuid("signal-multirate-fast");
        fast.period = .1;
        fast.code = "seen = input;";
        fast.inputs = {{port("signal-multirate-fast-in", "input", "s",
                             SignalScalarType::real, -1), {slow.id, slow.outputs[0].id}}};
        fast.outputs = {port("signal-multirate-fast-out", "seen", "s")};
        SignalIR multirate{{fast, slow}};
        auto multirate_state = initialize_signal_runtime(multirate);
        const auto multirate_emissions = run_signal_tasks(multirate, multirate_state, .3, {});
        check(multirate_emissions.size() == 6, "Multirate tasks execute on their own phase and period");
        std::vector<std::pair<double, double>> fast_samples;
        for (const auto &emission : multirate_emissions)
            if (emission.endpoint.object == fast.id)
                fast_samples.emplace_back(emission.value.time, emission.value.value);
        check(fast_samples.size() == 4, "Fast task executes four times through 0.3 s");
        for (std::size_t index = 0; index < fast_samples.size(); ++index)
            near(fast_samples[index].first, .1 * static_cast<double>(index), 1e-14,
                 "Fast task tick uses exact schedule");
        near(fast_samples[0].second, -1, 0, "Fast task reads initial value before slow task starts");
        near(fast_samples[1].second, .05, 1e-14, "Fast task reads first accepted slow output");
        near(fast_samples[2].second, .05, 1e-14, "Fast task holds slow output between ticks");
        near(fast_samples[3].second, .25, 1e-14, "Fast task reads second slow output");

        SignalTaskIR failing = slow;
        failing.phase = .35;
        failing.code = "out = 1 / 0;";
        auto failing_state = initialize_signal_runtime({{failing}});
        try {
            (void)run_signal_tasks({{failing}}, failing_state, .35, {});
            throw std::runtime_error("Expected runtime diagnostic from code block");
        } catch (const Diagnostic &diagnostic) {
            check(diagnostic.code == "invalid_code_block" && diagnostic.object == failing.id &&
                      std::abs(diagnostic.time - .35) < 1e-14,
                  "Runtime failure identifies the block and its scheduled tick");
        }
        failing.code = "while (true) {}";
        failing_state = initialize_signal_runtime({{failing}});
        error("invalid_code_block", [&] { (void)run_signal_tasks({{failing}}, failing_state, .35, {}); });

        const auto sample_sensor = derived_uuid("signal-hold-sensor");
        const auto sample_trigger = derived_uuid("signal-hold-trigger");
        SignalTaskIR hold;
        hold.id = derived_uuid("signal-hold-task");
        hold.period = .1;
        hold.code = "static double held = 0;\nif (sample) held = in;\nout = held;";
        hold.inputs = {{port("signal-hold-in", "in", ""), {sample_sensor, "out"}},
                       {port("signal-hold-sample", "sample", "", SignalScalarType::boolean),
                        {sample_trigger, "out"}}};
        hold.outputs = {port("signal-hold-out", "out", "")};
        SignalIR hold_ir{{hold}};
        auto hold_state = initialize_signal_runtime(hold_ir);
        auto sample_hold = [&](double time, double input, bool sample) {
            SignalFrame accepted{
                {signal_endpoint_key({sample_sensor, "out"}),
                 {SignalScalarType::real, "", input, time, true}},
                {signal_endpoint_key({sample_trigger, "out"}),
                 {SignalScalarType::boolean, "", sample ? 1.0 : 0.0, time, true}},
            };
            const auto values = run_signal_tasks(hold_ir, hold_state, time, accepted);
            check(values.size() == 1, "Sample-and-hold emits once per tick");
            return values[0].value.value;
        };
        near(sample_hold(0, 7, false), 0, 0, "Sample-and-hold starts at zero");
        near(sample_hold(.1, 3, true), 3, 0, "Sample-and-hold captures enabled input");
        near(sample_hold(.2, 8, false), 3, 0, "Sample-and-hold retains its value while disabled");
        near(sample_hold(.3, -2, true), -2, 0, "Sample-and-hold captures a later input");
        check(!hold_state.tasks.at(hold.id).program_state.static_values.empty(),
              "Sample-and-hold stores its retained value in serializable C state");

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

        Project motor_signal;
        motor_signal.id = derived_uuid("motor-speed-signal-project");
        motor_signal.wired = true;
        motor_signal.profile = {.02, .001, Method::backward_euler};
        Component motor_supply;
        motor_supply.id = derived_uuid("motor-speed-supply");
        motor_supply.kind = Kind::voltage;
        motor_supply.value = 10;
        Component motor;
        motor.id = derived_uuid("motor-speed-motor");
        motor.kind = Kind::dc_motor;
        motor.value = 2;
        motor.motor = {.1, .1, .01, .001, .02};
        motor_signal.components = {motor_supply, motor};
        const auto motor_ground = derived_uuid("motor-speed-ground");
        motor_signal.nodes.push_back({motor_ground, "GND", true});
        CodeBlock speed_reader;
        speed_reader.id = derived_uuid("motor-speed-reader");
        speed_reader.name = "Speed reader";
        speed_reader.period = .001;
        speed_reader.code = "out = speed;";
        speed_reader.inputs = {{derived_uuid("motor-speed-input"), "speed", "rad/s",
                                SignalScalarType::real, 0}};
        speed_reader.outputs = {{derived_uuid("motor-speed-output"), "out", "rad/s",
                                 SignalScalarType::real, 0}};
        motor_signal.code_blocks.push_back(speed_reader);
        PlotBlock speed_plot;
        speed_plot.id = derived_uuid("motor-speed-plot");
        speed_plot.inputs = 1;
        motor_signal.plots.push_back(speed_plot);
        motor_signal.wires = {
            {derived_uuid("motor-speed-wire-p"), {motor_supply.id, "p"}, {motor.id, "p"}},
            {derived_uuid("motor-speed-wire-supply-n"), {motor_supply.id, "n"}, {motor_ground, "node"}},
            {derived_uuid("motor-speed-wire-motor-n"), {motor.id, "n"}, {motor_ground, "node"}},
            {derived_uuid("motor-speed-wire-input"), {motor.id, "speed"},
             {speed_reader.id, speed_reader.inputs[0].id}},
            {derived_uuid("motor-speed-wire-plot"), {motor.id, "speed"}, {speed_plot.id, "in1"}},
        };
        const auto speed_type = port_type(motor_signal, {motor.id, "speed"});
        check(speed_type.domain == Domain::signal && speed_type.direction == Direction::output &&
                  plot_source_channels(motor_signal, speed_plot.id) == std::vector<std::string>{"omega/" + motor.id},
              "Motor speed is a signal source for CodeBlock and Plot");
        const auto motor_ir = compile(motor_signal);
        check(motor_ir.signal_inputs.size() == 1 &&
                  motor_ir.signal_inputs[0].source == SignalInputSource::unknown &&
                  motor_ir.unknowns[motor_ir.signal_inputs[0].index].object == "omega/" + motor.id,
              "Motor speed input reads the mechanical MNA unknown");
        const auto motor_run = execute(motor_ir);
        const auto speed_output = signal_endpoint_key({speed_reader.id, speed_reader.outputs[0].id});
        const auto speed_channel = std::find_if(motor_run.channels.begin(), motor_run.channels.end(),
                                                [&](const Channel &channel) { return channel.object == speed_output; });
        check(speed_channel != motor_run.channels.end(), "Motor speed CodeBlock output is recorded");
        check(motor_run.samples.back().values[size_t(speed_channel - motor_run.channels.begin())] > 0,
              "CodeBlock receives nonzero accepted motor speed");
        motor_signal.code_blocks[0].inputs[0].unit = "";
        error("incompatible_signal_value", [&] { (void)compile_signal_ir(motor_signal); });

        Project operators;
        operators.id = derived_uuid("signal-operator-project");
        operators.wired = true;
        auto source = [&](const std::string &key, const char *code, SignalScalarType type) {
            CodeBlock result;
            result.id = derived_uuid("signal-operator-" + key);
            result.name = key;
            result.period = 100e-6;
            result.code = code;
            result.outputs = {{derived_uuid("signal-operator-" + key + "-out"), "out", "", type, 0}};
            return result;
        };
        auto real_a = source("real-a", "out = 2;", SignalScalarType::real);
        auto real_b = source("real-b", "out = -3;", SignalScalarType::real);
        auto bool_true = source("bool-true", "out = 1;", SignalScalarType::boolean);
        auto bool_false = source("bool-false", "out = 0;", SignalScalarType::boolean);
        auto operator_block = [&](const std::string &key, const char *code,
                                  std::vector<CodePort> inputs, SignalScalarType output_type) {
            CodeBlock result;
            result.id = derived_uuid("signal-operator-" + key);
            result.name = key;
            result.period = 100e-6;
            result.code = code;
            result.inputs = std::move(inputs);
            result.outputs = {{derived_uuid("signal-operator-" + key + "-out"), "out", "", output_type, 0}};
            return result;
        };
        auto sum = operator_block("sum", "out = a + b;", {
                                      {derived_uuid("signal-operator-sum-a"), "a", "", SignalScalarType::real, 0},
                                      {derived_uuid("signal-operator-sum-b"), "b", "", SignalScalarType::real, 0},
                                  }, SignalScalarType::real);
        auto limiter = operator_block("limiter", "out = clamp(in, -1, 1);", {
                                          {derived_uuid("signal-operator-limiter-in"), "in", "", SignalScalarType::real, 0},
                                      }, SignalScalarType::real);
        auto comparator = operator_block("comparator", "out = a >= b;", {
                                             {derived_uuid("signal-operator-comparator-a"), "a", "", SignalScalarType::real, 0},
                                             {derived_uuid("signal-operator-comparator-b"), "b", "", SignalScalarType::real, 0},
                                         }, SignalScalarType::boolean);
        auto logical_and = operator_block("and", "out = a && b;", {
                                              {derived_uuid("signal-operator-and-a"), "a", "", SignalScalarType::boolean, 0},
                                              {derived_uuid("signal-operator-and-b"), "b", "", SignalScalarType::boolean, 0},
                                          }, SignalScalarType::boolean);
        operators.code_blocks = {real_a, real_b, bool_true, bool_false, sum, limiter, comparator, logical_and};
        auto signal_wire = [&](const std::string &key, const CodeBlock &from, const CodeBlock &to, size_t input) {
            operators.wires.push_back({derived_uuid("signal-operator-wire-" + key),
                                       {from.id, from.outputs[0].id}, {to.id, to.inputs[input].id}});
        };
        signal_wire("sum-a", real_a, sum, 0);
        signal_wire("sum-b", real_b, sum, 1);
        signal_wire("limiter", real_a, limiter, 0);
        signal_wire("comparator-a", real_a, comparator, 0);
        signal_wire("comparator-b", real_b, comparator, 1);
        signal_wire("and-a", bool_true, logical_and, 0);
        signal_wire("and-b", bool_false, logical_and, 1);
        const auto operator_ir = compile_signal_ir(operators);
        check(operator_ir.tasks.size() == operators.code_blocks.size(),
              "Operator presets compile through the ordinary Signal IR");
        auto operator_state = initialize_signal_runtime(operator_ir);
        (void)run_signal_tasks(operator_ir, operator_state, 0, {});
        auto operator_value = [&](const CodeBlock &block) {
            return operator_state.outputs.at(signal_endpoint_key({block.id, block.outputs[0].id}));
        };
        near(operator_value(sum).value, 0, 1e-15,
             "Sum uses its initial inputs on the first simultaneous Signal frame");
        (void)run_signal_tasks(operator_ir, operator_state, 100e-6, {});
        near(operator_value(sum).value, -1, 1e-15, "Sum reads both real inputs from the previous accepted frame");
        near(operator_value(limiter).value, 1, 1e-15, "Limiter uses the default -1..1 clamp");
        check(operator_value(comparator).type == SignalScalarType::boolean && operator_value(comparator).value == 1,
              "Comparator produces a boolean output");
        check(operator_value(logical_and).type == SignalScalarType::boolean && operator_value(logical_and).value == 0,
              "AND accepts boolean inputs and produces a boolean output");
        auto missing_operator_input = operators;
        std::erase_if(missing_operator_input.wires, [&](const Wire &wire) {
            return wire.to == Endpoint{sum.id, sum.inputs[0].id};
        });
        try {
            (void)compile_signal_ir(missing_operator_input);
            throw std::runtime_error("Expected missing Signal operator input diagnostic");
        } catch (const Diagnostic &diagnostic) {
            check(diagnostic.code == "missing_signal_source" && diagnostic.object == sum.id,
                  "Unconnected operator input reports the operator UUID");
        }

        Project carrier_pwm;
        carrier_pwm.id = derived_uuid("carrier-pwm-project");
        carrier_pwm.wired = true;
        carrier_pwm.profile.step = 100e-6;
        carrier_pwm.profile.stop = 1e-3;
        const auto carrier_ground = derived_uuid("carrier-pwm-ground");
        const auto carrier_node = derived_uuid("carrier-pwm-node");
        carrier_pwm.nodes = {{carrier_ground, "GND", true}, {carrier_node, "supply"}};
        Component carrier_supply;
        carrier_supply.id = derived_uuid("carrier-pwm-supply");
        carrier_supply.name = "V1";
        carrier_supply.kind = Kind::voltage;
        carrier_supply.value = 1;
        Component carrier_load;
        carrier_load.id = derived_uuid("carrier-pwm-load");
        carrier_load.name = "R1";
        carrier_load.kind = Kind::resistor;
        carrier_load.value = 1;
        carrier_pwm.components = {carrier_supply, carrier_load};
        CodeBlock carrier;
        carrier.id = derived_uuid("carrier-pwm-carrier");
        carrier.name = "Carrier generator";
        carrier.period = 100e-6;
        carrier.code = "double phase = t * 1000 - floor(t * 1000);\nout = 1 - 4 * abs(phase - 0.5);";
        carrier.outputs = {{derived_uuid("carrier-pwm-carrier-out"), "out", "", SignalScalarType::real, 0}};
        CodeBlock reference;
        reference.id = derived_uuid("carrier-pwm-reference");
        reference.name = "Reference";
        reference.period = 100e-6;
        reference.code = "out = 0;";
        reference.outputs = {{derived_uuid("carrier-pwm-reference-out"), "out", "", SignalScalarType::real, 0}};
        CodeBlock pwm_comparator;
        pwm_comparator.id = derived_uuid("carrier-pwm-comparator");
        pwm_comparator.name = "PWM comparator";
        pwm_comparator.period = 100e-6;
        pwm_comparator.code = "out = reference >= carrier;";
        pwm_comparator.inputs = {{derived_uuid("carrier-pwm-reference-in"), "reference", "", SignalScalarType::real, 0},
                                 {derived_uuid("carrier-pwm-carrier-in"), "carrier", "", SignalScalarType::real, 0}};
        pwm_comparator.outputs = {{derived_uuid("carrier-pwm-comparator-out"), "out", "", SignalScalarType::boolean, 0}};
        carrier_pwm.code_blocks = {carrier, reference, pwm_comparator};
        carrier_pwm.wires = {
            {derived_uuid("carrier-pwm-supply-p"), {carrier_supply.id, "p"}, {carrier_node, "node"}},
            {derived_uuid("carrier-pwm-supply-n"), {carrier_supply.id, "n"}, {carrier_ground, "node"}},
            {derived_uuid("carrier-pwm-load-p"), {carrier_load.id, "p"}, {carrier_node, "node"}},
            {derived_uuid("carrier-pwm-load-n"), {carrier_load.id, "n"}, {carrier_ground, "node"}},
            {derived_uuid("carrier-pwm-reference-wire"), {reference.id, reference.outputs[0].id},
             {pwm_comparator.id, pwm_comparator.inputs[0].id}},
            {derived_uuid("carrier-pwm-carrier-wire"), {carrier.id, carrier.outputs[0].id},
             {pwm_comparator.id, pwm_comparator.inputs[1].id}},
        };
        const auto carrier_ir = compile(carrier_pwm);
        check(carrier_ir.signal.tasks.size() == 3 && carrier_ir.signal_gates.empty(),
              "Carrier and PWM comparator use the ordinary Signal IR without a gate-specific model");
        const auto carrier_run = execute(carrier_ir);
        const auto carrier_channel = std::find_if(carrier_run.channels.begin(), carrier_run.channels.end(), [&](const Channel &channel) {
            return channel.object == endpoint_key({carrier.id, carrier.outputs[0].id});
        });
        const auto comparator_gate = std::find(carrier_run.gate_objects.begin(), carrier_run.gate_objects.end(),
                                               endpoint_key({pwm_comparator.id, pwm_comparator.outputs[0].id}));
        check(carrier_channel != carrier_run.channels.end() && comparator_gate != carrier_run.gate_objects.end(),
              "Carrier and PWM comparator outputs are recorded by their typed Signal ports");
        const auto carrier_index = static_cast<std::size_t>(carrier_channel - carrier_run.channels.begin());
        const auto comparator_index = static_cast<std::size_t>(comparator_gate - carrier_run.gate_objects.begin());
        auto carrier_sample = [&](double time) -> const Sample & {
            const auto found = std::find_if(carrier_run.samples.begin(), carrier_run.samples.end(), [&](const Sample &sample) {
                return std::abs(sample.time - time) < 1e-13;
            });
            if (found == carrier_run.samples.end())
                throw std::runtime_error("Missing carrier sample");
            return *found;
        };
        near(carrier_sample(0).values[carrier_index], -1, 1e-12, "Carrier starts at -1");
        near(carrier_sample(.2e-3).values[carrier_index], -.2, 1e-12, "Carrier rises on the first quarter");
        near(carrier_sample(.5e-3).values[carrier_index], 1, 1e-12, "Carrier reaches +1 halfway through its period");
        near(carrier_sample(.8e-3).values[carrier_index], -.2, 1e-12, "Carrier falls symmetrically after the peak");
        near(carrier_sample(1e-3).values[carrier_index], -1, 1e-12, "Carrier repeats after one millisecond");
        check(carrier_sample(.3e-3).gates[comparator_index] &&
                  !carrier_sample(.4e-3).gates[comparator_index],
              "PWM comparator reads the previous accepted carrier frame through its wired Signal input");

        SignalTaskIR thyristor_firing;
        thyristor_firing.id = derived_uuid("three-phase-thyristor-code-block");
        thyristor_firing.period = 10e-6;
        thyristor_firing.code = R"(double delay = ramp(0, 10, 0.008333333333, 0.001111111111);
gAp = phasepwm(50, 0.02, delay);
gAm = phasepwm(50, 0.02, delay + 0.010000000000);
gBp = phasepwm(50, 0.02, delay + 0.006666666667);
gBm = phasepwm(50, 0.02, delay + 0.016666666667);
gCp = phasepwm(50, 0.02, delay + 0.013333333333);
gCm = phasepwm(50, 0.02, delay + 0.023333333333);)";
        for (const auto *name : {"gAp", "gAm", "gBp", "gBm", "gCp", "gCm"})
            thyristor_firing.outputs.push_back(port(std::string("thyristor-firing-") + name, name, "",
                                                    SignalScalarType::boolean));
        SignalIR firing_ir{{thyristor_firing}};
        auto firing_state = initialize_signal_runtime(firing_ir);
        auto check_firing = [&](double time, int active) {
            const auto outputs = run_signal_tasks(firing_ir, firing_state, time, {});
            std::vector<double> last(6, -1);
            for (const auto &output : outputs)
                for (size_t index = 0; index < thyristor_firing.outputs.size(); ++index)
                    if (output.endpoint.port == thyristor_firing.outputs[index].id)
                        last[index] = output.value.value;
            for (size_t index = 0; index < last.size(); ++index)
                check(last[index] == (int(index) == active ? 1. : 0.),
                      "Three-phase thyristor CodeBlock emits one pulse on the expected bool output");
        };
        check_firing(0, -1);
        check_firing(.0085, 0);
        check_firing(.0118, 5);
        check_firing(.0152, 2);
        check_firing(.0185, 1);
        check_firing(.0219, 4);
        check_firing(.0252, 3);

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
        auto stale_tick = restored;
        stale_tick.signal_tasks.at(controller.id).next_tick = 0;
        error("invalid_snapshot", [&] { validate_snapshot(stale_tick, integrated_ir); });
        auto missing_frame = restored;
        missing_frame.accepted_signal_inputs.erase(missing_frame.accepted_signal_inputs.begin());
        error("invalid_snapshot", [&] { validate_snapshot(missing_frame, integrated_ir); });
        std::cout << "PASS typed causal Signal IR and deterministic C scheduler\n";
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
