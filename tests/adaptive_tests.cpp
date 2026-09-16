#include "core/model/waveform.hpp"
#include "core/solver/reference/reference.hpp"
#include "formats/project/project.hpp"
#include "formats/snapshot/snapshot.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
using namespace pds;
namespace {
void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
Project load(const std::string &root, const char *name) {
    std::ifstream input(root + "/examples/" + name + ".pds");
    return read_project(input);
}
void equal(const Sample &a, const Sample &b) {
    check(a.time == b.time && a.values == b.values && a.gates == b.gates,
          "Adaptive resume changed accepted trajectory");
}
Result checkpoint(const SimulationIR &ir, const SimulationSnapshot *state = nullptr, size_t limit = 0) {
    ExecutionOptions options{state, true, limit};
    return execute(ir, nullptr, nullptr, nullptr, nullptr, {}, &options);
}
} // namespace
int main(int argc, char **argv) try {
    check(argc == 2, "Source directory required");
    const std::string root(argv[1]);
    for (auto method : {Method::backward_euler, Method::trapezoidal}) {
        auto p = load(root, "rc");
        p.profile.method = method;
        p.profile.step = .001;
        auto &control = p.profile.step_control;
        control.adaptive = true;
        double previous_error = 1;
        size_t previous_steps = 0;
        for (double tolerance : {1e-3, 1e-5}) {
            control.relative_tolerance = tolerance;
            control.voltage_tolerance = tolerance * .01;
            control.current_tolerance = tolerance * 1e-5;
            const auto r = execute(compile(p));
            size_t output = 0;
            for (size_t i = 0; i < r.channels.size(); ++i)
                if (r.channels[i].name == "u:output")
                    output = i;
            double error = 0;
            for (const auto &sample : r.samples)
                error =
                    std::max(error, std::abs(sample.values[output] - (1 - std::exp(-sample.time / .001))));
            std::cout << method_name(method) << " tolerance=" << tolerance << " steps=" << r.accepted_steps
                      << " rejected=" << r.rejected_steps << " error=" << error << '\n';
            check(r.rejected_steps > 0 && r.min_accepted_step < r.max_accepted_step,
                  "Adaptive controller did not vary step");
            check(r.max_local_error <= 1 && r.max_accepted_step <= p.profile.step,
                  "Adaptive acceptance bound");
            check(error < previous_error / 3 && r.accepted_steps > previous_steps,
                  "Tighter tolerance did not improve solution");
            previous_error = error;
            previous_steps = r.accepted_steps;
        }
        p.profile.step_control.relative_tolerance = 0;
        check(execute(compile(p)).max_local_error <= 1, "Absolute-only adaptive tolerance");
        p.profile.step_control.minimum_step = p.profile.step;
        try {
            execute(compile(p));
            check(false, "Minimum-step error expected");
        } catch (const Diagnostic &e) {
            check(e.code == "adaptive_step_limit" && !e.object.empty(), "Wrong minimum-step diagnostic");
        }
    }
    for (auto method : {Method::backward_euler, Method::trapezoidal}) {
        Project lc;
        lc.id = derived_uuid("adaptive-lc");
        const auto g = derived_uuid("adaptive-ground"), n = derived_uuid("adaptive-node");
        lc.nodes = {{g, "ground", true}, {n, "output"}};
        lc.components = {{derived_uuid("adaptive-l"), "L", Kind::inductor, n, g, .01},
                         {derived_uuid("adaptive-c"), "C", Kind::capacitor, n, g, .001, 1}};
        lc.profile.stop = .1;
        lc.profile.step = .003;
        lc.profile.method = method;
        lc.profile.step_control.adaptive = true;
        lc.profile.step_control.relative_tolerance = 1e-5;
        const auto r = execute(compile(lc));
        size_t voltage = 0, current = 0;
        for (size_t k = 0; k < r.channels.size(); ++k) {
            if (r.channels[k].object == n)
                voltage = k;
            if (r.channels[k].object == lc.components.front().id)
                current = k;
        }
        double previous_energy = .0005, maximum_error = 0;
        for (const auto &sample : r.samples) {
            const double u = sample.values[voltage], i = sample.values[current];
            const double energy = .5 * (.001 * u * u + .01 * i * i);
            if (method == Method::trapezoidal)
                check(std::abs(energy - .0005) < 1e-12, "Variable-step LC energy");
            else
                check(energy <= previous_energy + 1e-12, "BE adaptive LC gained energy");
            previous_energy = energy;
            maximum_error = std::max(maximum_error, std::abs(u - std::cos(sample.time / std::sqrt(.00001))));
        }
        std::cout << "LC " << method_name(method) << " max error=" << maximum_error << '\n';
        check(maximum_error < (method == Method::trapezoidal ? .002 : .06),
              "Adaptive LC phase/amplitude error");
    }
    for (const char *name :
         {"rc-pulse", "diode-freewheel", "diode-recovery", "thyristor-halfwave", "ac-voltage-controller"}) {
        for (auto method : {Method::backward_euler, Method::trapezoidal}) {
            auto p = load(root, name);
            p.profile.method = method;
            p.profile.step_control.adaptive = true;
            const auto ir = compile(p);
            const auto full = checkpoint(ir);
            check(full.last_time == p.profile.stop, "Adaptive end time");
            for (size_t i = 1; i < full.samples.size(); ++i)
                check(full.samples[i].time > full.samples[i - 1].time, "Nonmonotone accepted times");
            for (const auto &event : ir.events)
                check(std::any_of(full.samples.begin(), full.samples.end(),
                                  [&](const auto &s) { return s.time == event.time; }),
                      "Missed gate edge");
            for (const auto &stamp : ir.stamps) {
                double time = 0;
                while (true) {
                    const double edge = next_source_breakpoint(stamp.component, time);
                    if (edge > ir.profile.stop)
                        break;
                    check(edge > time, "Source breakpoint did not advance");
                    check(std::any_of(full.samples.begin(), full.samples.end(),
                                      [&](const auto &s) { return s.time == edge; }),
                          "Missed source edge");
                    time = edge;
                }
            }
            std::optional<SimulationSnapshot> state;
            size_t offset = 0, rejections = 0;
            do {
                const auto part = checkpoint(ir, state ? &*state : nullptr, 73);
                for (size_t k = 0; k < part.samples.size(); ++k)
                    equal(part.samples[k], full.samples.at(offset + k));
                offset += part.accepted_steps;
                rejections += part.rejected_steps;
                std::ostringstream saved;
                write_snapshot(*part.snapshot, saved);
                std::istringstream input(saved.str());
                state = read_snapshot(input);
            } while (state->time < p.profile.stop);
            check(offset == full.accepted_steps && rejections == full.rejected_steps,
                  "Adaptive checkpoint lost controller state");
            Recording none;
            none.all = false;
            const auto silent = execute(ir, nullptr, nullptr, &none);
            check(silent.samples.empty() && silent.accepted_steps == full.accepted_steps &&
                      silent.rejected_steps == full.rejected_steps,
                  "Recording changes adaptivity");
            std::ostringstream output;
            write_project(p, output);
            std::istringstream input(output.str());
            check(read_project(input).profile == p.profile, "Adaptive profile round trip");
            std::cout << "Adaptive events/checkpoint " << name << ' ' << method_name(method) << '\n';
        }
    }
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
