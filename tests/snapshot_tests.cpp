#include "core/solver/reference/reference.hpp"
#include "formats/project/project.hpp"
#include "formats/snapshot/snapshot.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string_view>
using namespace pds;
static void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
static Result run(const SimulationIR &ir, const SimulationSnapshot *state, size_t steps) {
    ExecutionOptions options{state, true, steps};
    return execute(ir, nullptr, nullptr, nullptr, nullptr, {}, &options);
}
static void rejected(const SimulationSnapshot &state, const SimulationIR &ir) {
    try {
        (void)run(ir, &state, 1);
    } catch (const Diagnostic &e) {
        check(e.code == "invalid_snapshot", "Wrong checkpoint diagnostic");
        return;
    }
    throw std::runtime_error("Invalid checkpoint was accepted");
}
static void compare(const Sample &a, const Sample &b) {
    check(a.time == b.time && a.values == b.values && a.gates == b.gates,
          "Resumed samples must be bit-identical to uninterrupted execution");
}
static void invalid_file(const std::string &text) {
    std::istringstream stream(text);
    try {
        (void)read_snapshot(stream);
    } catch (const Diagnostic &e) {
        check(e.code == "snapshot_format", "Wrong file diagnostic");
        return;
    }
    throw std::runtime_error("Malformed state file was accepted");
}
int main(int argc, char **argv) try {
    check(argc == 2, "Pass repository root");
    for (const char *name : {"rc", "rlc", "rc-pulse", "diode-freewheel", "diode-recovery",
                             "thyristor-halfwave", "ac-voltage-controller", "open-end-winding",
                             "gate-script-pwm"}) {
        std::ifstream file(std::string(argv[1]) + "/examples/" + name + ".pds");
        auto project = read_project(file);
        for (auto method : {Method::backward_euler, Method::trapezoidal}) {
            project.profile.method = method;
            auto ir = compile(project);
            const auto expected = execute(ir);
            std::optional<SimulationSnapshot> checkpoint;
            size_t offset = 0;
            do {
                const auto part = run(ir, checkpoint ? &*checkpoint : nullptr, 137);
                check(part.snapshot.has_value() && !part.cancelled,
                      "Partial run produces a valid checkpoint");
                for (size_t k = 0; k < part.samples.size(); ++k)
                    compare(part.samples[k], expected.samples.at(offset + k));
                offset += part.accepted_steps;
                std::ostringstream saved_state;
                write_snapshot(*part.snapshot, saved_state);
                std::istringstream restored(saved_state.str());
                checkpoint = read_snapshot(restored);
                check(checkpoint == part.snapshot, "State file preserves every bit of numeric state");
            } while (checkpoint->time < ir.profile.stop);
            check(offset == expected.accepted_steps, "Resume preserves the global step grid");
            if (std::string_view(name) == "rc" && method == Method::backward_euler) {
                auto signal_state = *checkpoint;
                signal_state.version = 4;
                const auto task_id = derived_uuid("snapshot-signal-task");
                SignalTaskSnapshot task;
                task.next_tick = 123;
                task.outputs["command"] = 2.5;
                task.program_state.static_values[1] = 1.25;
                task.program_state.initialized[1] = true;
                signal_state.signal_tasks[task_id] = task;
                signal_state.signal_outputs[task_id + "/out"] =
                    {SignalScalarType::real, "V", 2.5, signal_state.time, true};
                signal_state.accepted_signal_inputs[derived_uuid("snapshot-sensor") + "/out"] =
                    {SignalScalarType::boolean, "", 1, signal_state.time, true};
                std::ostringstream encoded;
                write_snapshot(signal_state, encoded);
                std::istringstream decoded(encoded.str());
                check(read_snapshot(decoded) == signal_state,
                      "Version 4 preserves signal ticks, C state, outputs and accepted frame");
                auto with_signal = ir;
                SignalTaskIR signal_task;
                signal_task.id = task_id;
                signal_task.code = "out = 2.5;";
                signal_task.outputs = {{derived_uuid("snapshot-signal-output"), "command", "V",
                                        SignalScalarType::real, 0}};
                with_signal.signal.tasks.push_back(signal_task);
                signal_state.signal_outputs.clear();
                signal_state.signal_outputs[task_id + "/" + signal_task.outputs[0].id] =
                    {SignalScalarType::real, "V", 2.5, signal_state.time, true};
                signal_state.accepted_signal_inputs.clear();
                signal_state.contract = snapshot_contract(with_signal, signal_state.time);
                validate_snapshot(signal_state, with_signal);
                const auto invalid_signal_state = [](const SimulationSnapshot &state,
                                                     const SimulationIR &model) {
                    try { validate_snapshot(state, model); }
                    catch (const Diagnostic &error) {
                        check(error.code == "invalid_snapshot", "Wrong signal snapshot diagnostic");
                        return;
                    }
                    throw std::runtime_error("Invalid signal snapshot was accepted");
                };
                auto edited_code = with_signal;
                edited_code.signal.tasks[0].code = "out = 3;";
                invalid_signal_state(signal_state, edited_code);
                auto damaged_signal = signal_state;
                damaged_signal.signal_tasks.at(task_id).outputs["command"] = 3;
                invalid_signal_state(damaged_signal, with_signal);
            }
            if (!ir.events.empty()) {
                auto changed = ir;
                changed.events.front().closed = !changed.events.front().closed;
                rejected(*checkpoint, changed);
            }
            std::ostringstream state_file;
            write_snapshot(*checkpoint, state_file);
            invalid_file(state_file.str().substr(0, state_file.str().size() / 2));
            invalid_file(state_file.str() + "unexpected");
            auto version = state_file.str();
            version.replace(0, version.find('\n'), "PowerDriveSimSnapshot 99");
            invalid_file(version);
            if(ir.gate_programs.empty()) {
                auto legacy = *checkpoint;
                legacy.version = 1;
                std::ostringstream old_file;
                write_snapshot(legacy, old_file);
                std::istringstream old_input(old_file.str());
                const auto old_state = read_snapshot(old_input);
                check(old_state == legacy, "Version 1 checkpoint migration");
                validate_snapshot(old_state, ir);
            }
            auto shortened = ir;
            shortened.profile.stop = expected.samples[97].time;
            const auto first = run(shortened, nullptr, 0);
            const auto rest = run(ir, &*first.snapshot, 0);
            for (size_t k = 0; k < rest.samples.size(); ++k)
                compare(rest.samples[k], expected.samples.at(97 + k));
            auto renamed = ir;
            for (auto &c : renamed.unknowns)
                c.name = "Renamed";
            for (auto &s : renamed.stamps) {
                s.component.name = "Renamed";
                s.component.x += 100;
            }
            check(run(renamed, &*first.snapshot, 1).accepted_steps == 1,
                  "Display changes do not invalidate state");
            auto changed = ir;
            changed.profile.step *= 2;
            rejected(*first.snapshot, changed);
            changed = ir;
            changed.stamps.front().component.value += 1;
            rejected(*first.snapshot, changed);
            auto damaged = *first.snapshot;
            damaged.states.clear();
            rejected(damaged, ir);
            damaged = *first.snapshot;
            damaged.values.front() = std::numeric_limits<double>::quiet_NaN();
            rejected(damaged, ir);
            damaged = *first.snapshot;
            ++damaged.next_grid;
            rejected(damaged, ir);
            Recording none{false, {}};
            ExecutionOptions capture{&*first.snapshot, true, 1};
            const auto silent = execute(ir, nullptr, nullptr, &none, nullptr, {}, &capture);
            check(silent.samples.empty() && silent.snapshot.has_value(),
                  "State capture does not require recording");
            check(select_result(first, {}).snapshot == first.snapshot,
                  "Channel selection preserves checkpoint");
            std::atomic_bool stop{true};
            ExecutionOptions cancelled_capture{nullptr, true, 0};
            const auto cancelled = execute(ir, &stop, nullptr, &none, nullptr, {}, &cancelled_capture);
            check(cancelled.cancelled && cancelled.snapshot && cancelled.snapshot->time == 0,
                  "Stop before first step still captures initialized state");
            check(run(ir, &*cancelled.snapshot, 1).accepted_steps == 1, "Initialized checkpoint resumes");
        }
        if (std::string_view(name) == "open-end-winding") {
            project.profile.method = Method::trapezoidal;
            project.profile.stop = .06;
            const auto ir = compile(project);
            const auto expected = execute(ir);
            const auto first = run(ir, nullptr, 19000);
            check(first.snapshot && first.snapshot->time > .037 && first.snapshot->time < .039,
                  "Long periodic run pauses before later Gate edges");
            const auto resumed = run(ir, &*first.snapshot, 0);
            for (size_t k = 0; k < resumed.samples.size(); ++k)
                compare(resumed.samples[k], expected.samples.at(first.accepted_steps + k));
        }
        std::cout << "PASS checkpoint " << name << '\n';
    }
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
