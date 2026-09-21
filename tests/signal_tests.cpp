#include "core/solver/reference/signal.hpp"

#include <cmath>
#include <functional>
#include <iostream>

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
        std::cout << "PASS typed causal Signal IR and deterministic C scheduler\n";
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
