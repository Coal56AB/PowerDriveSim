#include "core/model/semiconductor.hpp"
#include "core/solver/reference/reference.hpp"
#include "formats/project/project.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace pds;
namespace {
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
Project load(const std::filesystem::path &root, const char *name) {
    std::ifstream input(root / "examples" / name);
    return read_project(input);
}
size_t channel(const Result &r, const std::string &id) {
    const auto found =
        std::find_if(r.channels.begin(), r.channels.end(), [&](const auto &c) { return c.object == id; });
    check(found != r.channels.end(), "Missing channel");
    return static_cast<size_t>(found - r.channels.begin());
}
Project branch(Kind device) {
    Project p;
    p.id = derived_uuid("initial-state-project");
    p.name = "Initial state";
    const auto ground = derived_uuid("initial-ground"), supply = derived_uuid("initial-supply"),
               node = derived_uuid("initial-node");
    p.nodes = {{ground, "GND", true}, {supply, "supply"}, {node, "load"}};
    p.components = {{derived_uuid("initial-v"), "V", Kind::voltage, supply, ground, 5},
                    {derived_uuid("initial-r"), "R", Kind::resistor, supply, node, 10},
                    {derived_uuid("initial-device"), "device", device, node, ground, 0}};
    p.profile.stop = .002;
    p.profile.step = 1e-5;
    return p;
}
void rejected(const Project &p, const std::string &code) {
    try {
        (void)execute(compile(p));
    } catch (const Diagnostic &e) {
        check(e.code == code, e.what());
        check(!e.object.empty(), "Missing diagnostic address");
        return;
    }
    throw std::runtime_error("Expected initial-state diagnostic");
}
} // namespace
int main(int argc, char **argv) {
    try {
        check(argc == 2, "Source directory required");
        const std::filesystem::path root(argv[1]);
        for (auto method : {Method::backward_euler, Method::trapezoidal}) {
            for (auto kind : {Kind::diode, Kind::ideal_switch, Kind::thyristor, Kind::igbt}) {
                for (auto model : {SemiconductorModel::ideal, SemiconductorModel::piecewise_linear}) {
                    auto p = branch(kind);
                    auto &device = p.components.back();
                    device.semiconductor.model = model;
                    device.closed = gate_controlled(kind);
                    p.profile.initial_state = InitialState::dc_operating_point;
                    p.profile.method = method;
                    if (kind == Kind::thyristor)
                        p.events.push_back({2e-5, device.id, false});
                    const auto result = execute(compile(p));
                    const double resistance =
                        10 + (model == SemiconductorModel::ideal ? 0 : device.semiconductor.ron);
                    const double drop = model == SemiconductorModel::ideal || kind == Kind::ideal_switch
                                            ? 0
                                            : device.semiconductor.forward_voltage *
                                                  (1 - device.semiconductor.ron / device.semiconductor.roff);
                    for (const auto &sample : result.samples)
                        check(std::abs(sample.values[channel(result, device.id)] - (5 - drop) / resistance) <
                                  1e-10,
                              "DC semiconductor equilibrium/thyristor hold");
                }
            }
            for (auto kind : {Kind::capacitor, Kind::inductor}) {
                auto p = branch(kind);
                auto &c = p.components.back();
                c.value = kind == Kind::capacitor ? 1e-4 : .01;
                c.initial = kind == Kind::capacitor ? 2 : .2;
                p.profile.method = method;
                p.profile.initial_state = InitialState::dc_operating_point;
                ExecutionOptions options;
                options.capture_snapshot = true;
                auto r = execute(compile(p), nullptr, nullptr, nullptr, nullptr, {}, &options);
                const auto i = channel(r, c.id), u = channel(r, c.positive);
                for (const auto &sample : r.samples) {
                    check(std::abs(sample.values[i] - (kind == Kind::capacitor ? 0 : .5)) < 1e-10,
                          "DC current drift");
                    check(std::abs(sample.values[u] - (kind == Kind::capacitor ? 5 : 0)) < 1e-10,
                          "DC voltage drift");
                }
                validate_snapshot(*r.snapshot, compile(p));
                p.profile.initial_state = InitialState::zero;
                r = execute(compile(p));
                check(std::abs(r.samples.front().values[kind == Kind::capacitor ? u : i]) < 1e-12,
                      "Zero initial state");
                p.profile.initial_state = InitialState::specified;
                r = execute(compile(p));
                check(std::abs(r.samples.front().values[kind == Kind::capacitor ? u : i] - c.initial) < 1e-12,
                      "Specified initial state");
            }
            auto p = branch(Kind::diode);
            auto &d = p.components.back();
            d.semiconductor.model = SemiconductorModel::piecewise_linear;
            d.semiconductor.charge_dynamics = true;
            d.semiconductor.initial_charge = 1e-3;
            p.profile.initial_state = InitialState::dc_operating_point;
            p.profile.method = method;
            p.profile.step = 1e-7;
            p.profile.stop = 1e-5;
            ExecutionOptions options;
            options.capture_snapshot = true;
            auto r = execute(compile(p), nullptr, nullptr, nullptr, nullptr, {}, &options);
            for (const auto &sample : r.samples)
                for (size_t i = 0; i < sample.values.size(); ++i)
                    check(std::abs(sample.values[i] - r.samples.front().values[i]) < 1e-9,
                          "DC diode charge not stationary");
            const auto ir = compile(p);
            const auto index = std::find_if(ir.stamps.begin(), ir.stamps.end(),
                                            [&](const auto &s) { return s.component.id == d.id; }) -
                               ir.stamps.begin();
            const auto law = diode_charge_law(d.semiconductor);
            const auto voltage = r.samples.back().values[channel(r, d.positive)];
            check(std::abs(r.snapshot->states[index] -
                           law.alpha * (voltage - d.semiconductor.forward_voltage) / law.lambda) < 1e-12,
                  "DC stored charge");

            auto warm = load(root, "rc.pds");
            warm.profile.method = method;
            const auto full = execute(compile(warm));
            warm.profile.warmup = full.samples[117].time;
            auto tail = execute(compile(warm));
            check(tail.samples.size() == full.samples.size() - 117, "Warm-up sample count");
            for (size_t k = 0; k < tail.samples.size(); ++k)
                check(tail.samples[k].time == full.samples[k + 117].time &&
                          tail.samples[k].values == full.samples[k + 117].values,
                      "Warm-up changed physics");
            warm.profile.warmup += warm.profile.step * .37;
            tail = execute(compile(warm));
            check(tail.samples.front().time == warm.profile.warmup, "Off-grid warm-up boundary");
            std::ostringstream output;
            write_project(warm, output);
            std::istringstream input(output.str());
            check(read_project(input).profile == warm.profile, "Initial profile round trip");
            Recording none;
            none.all = false;
            options.max_steps = 10;
            auto part = execute(compile(warm), nullptr, nullptr, &none, nullptr, {}, &options);
            check(part.samples.empty() && part.snapshot && part.last_time < warm.profile.warmup,
                  "Warm-up stepping snapshot");
            options.resume = &*part.snapshot;
            options.max_steps = 0;
            const auto resumed = execute(compile(warm), nullptr, nullptr, nullptr, nullptr, {}, &options);
            check(resumed.samples.size() == tail.samples.size(), "Resume warm-up size");
            for (size_t k = 0; k < tail.samples.size(); ++k)
                check(resumed.samples[k].values == tail.samples[k].values, "Resume warm-up values");
        }
        auto p = branch(Kind::capacitor);
        p.components.back().value = 1e-4;
        p.components.erase(p.components.begin(), p.components.begin() + 2);
        p.nodes.erase(p.nodes.begin() + 1);
        p.profile.initial_state = InitialState::dc_operating_point;
        rejected(p, "floating_island");
        auto source = p.components.back();
        source.id = derived_uuid("initial-current");
        source.kind = Kind::current;
        source.value = 1;
        p.components.push_back(source);
        rejected(p, "current_cutset");
        p.components.front().kind = Kind::inductor;
        p.components.front().value = .01;
        p.components.back().kind = Kind::voltage;
        rejected(p, "conflicting_voltage_constraints");
        p = load(root, "rc.pds");
        p.profile.warmup = p.profile.stop;
        rejected(p, "invalid_profile");
        p = branch(Kind::thyristor);
        p.components.back().semiconductor.initial_latched = true;
        p.profile.initial_state = InitialState::zero;
        const auto zero_latch = execute(compile(p));
        check(zero_latch.samples.front().values[channel(zero_latch, p.components.back().id)] == 0,
              "Zero mode retained thyristor latch");
        std::cout << "Initial states, DC equilibrium, warm-up, snapshots and diagnostics passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
