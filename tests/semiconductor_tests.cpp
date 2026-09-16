#include "core/editor/document.hpp"
#include "core/editor/properties.hpp"
#include "core/model/hierarchy.hpp"
#include "core/model/semiconductor.hpp"
#include "core/solver/reference/reference.hpp"
#include "formats/project/project.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <sstream>
using namespace pds;
static std::string id(int n) {
    return derived_uuid("semiconductor-test/" + std::to_string(n));
}
static void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
static void near(double value, double expected, double tolerance, const char *message) {
    if (!std::isfinite(value) || std::abs(value - expected) > tolerance)
        throw std::runtime_error(std::string(message) + ": " + std::to_string(value) + " vs " +
                                 std::to_string(expected));
}
static void error(const char *code, const std::function<void()> &call) {
    try {
        call();
    } catch (const Diagnostic &e) {
        check(e.code == code, "Wrong diagnostic code");
        return;
    }
    throw std::runtime_error(std::string("Missing diagnostic ") + code);
}
static double value(const Result &r, const Sample &s, const std::string &object) {
    auto channel =
        std::find_if(r.channels.begin(), r.channels.end(), [&](const auto &c) { return c.object == object; });
    check(channel != r.channels.end(), "Channel missing");
    return s.values[size_t(channel - r.channels.begin())];
}
static Project fixture(Kind device = Kind::diode) {
    Project p;
    p.id = id(0);
    p.name = "PWL test";
    p.profile = {.01, .001};
    p.nodes = {{id(1), "Ground", true}, {id(2), "Input", false}, {id(3), "Output", false}};
    p.components = {{id(10), "Supply", Kind::voltage, id(2), id(1), 5},
                    {id(11), "Device", device, id(2), id(3), 0},
                    {id(12), "Load", Kind::resistor, id(3), id(1), 10}};
    p.components[1].semiconductor = {SemiconductorModel::piecewise_linear, .2, 1000, .7};
    return p;
}
static std::string encode(const Project &p) {
    std::ostringstream out;
    write_project(p, out);
    return out.str();
}
static void roundtrip(const Project &p) {
    std::istringstream in(encode(p));
    check(read_project(in) == p, "PWL round trip");
}
int main(int argc, char **argv) try {
    check(argc == 2, "Pass repository root");
    for (auto kind : {Kind::diode, Kind::ideal_switch})
        for (auto method : {Method::backward_euler, Method::trapezoidal})
            for (bool closed : {false, true})
                for (double voltage : {-5., 0., .69, .7, .707, .72, 5.}) {
                    auto p = fixture(kind);
                    p.profile.method = method;
                    p.components[0].value = voltage;
                    p.components[1].closed = kind == Kind::ideal_switch && closed;
                    const auto &model = p.components[1].semiconductor;
                    const bool on = kind == Kind::ideal_switch
                                        ? closed
                                        : voltage > model.forward_voltage * (1 + 10 / model.roff);
                    const double intercept =
                        kind == Kind::diode && on ? model.forward_voltage * (1 - model.ron / model.roff) : 0;
                    const double expected = (voltage - intercept) / (10 + (on ? model.ron : model.roff));
                    auto r = execute(compile(p));
                    for (const auto &s : r.samples) {
                        const auto current = value(r, s, id(11)), output = value(r, s, id(3));
                        near(current, expected, 1e-11, "Analytical device current");
                        near(output, expected * 10, 1e-11, "Analytical load voltage");
                        const double device_power = (voltage - output) * current;
                        check(device_power >= -1e-12, "Passive PWL device cannot generate power");
                        near(voltage * value(r, s, id(10)) + output * output / 10 + device_power, 0, 1e-11,
                             "Instantaneous power balance");
                    }
                    roundtrip(p);
                }
    // Independent RC solution while the diode stays on, and leakage discharge while it stays off.
    for (auto method : {Method::backward_euler, Method::trapezoidal}) {
        auto rc = fixture();
        rc.profile = {.01, 1e-6, method};
        rc.nodes.push_back({id(4), "Capacitor", false});
        rc.components[2].negative = id(4);
        rc.components.push_back({id(13), "C", Kind::capacitor, id(4), id(1), .001});
        for (bool charge : {true, false}) {
            rc.components[0].value = charge ? 5 : 0;
            rc.components.back().initial = charge ? 0 : 2;
            const auto result = execute(compile(rc));
            const double tau = (10 + (charge ? .2 : 1000)) * .001;
            double maximum = 0, energy = 0;
            for (size_t i = 0; i < result.samples.size(); ++i) {
                const auto &sample = result.samples[i];
                const double expected = charge ? (5 - .7 * (1 - .2 / 1000)) * (1 - std::exp(-sample.time / tau))
                                               : 2 * std::exp(-sample.time / tau);
                maximum = std::max(maximum, std::abs(value(result, sample, id(4)) - expected));
                auto capacitor_power = [&](const Sample &s) {
                    const double input = value(result, s, id(2)), middle = value(result, s, id(3)),
                                 output = value(result, s, id(4)), current = value(result, s, id(11));
                    return -input * value(result, s, id(10)) - (input - middle) * current -
                           (middle - output) * (middle - output) / 10;
                };
                if (i) energy += (sample.time - result.samples[i - 1].time) *
                                  (capacitor_power(sample) + capacitor_power(result.samples[i - 1])) / 2;
            }
            check(maximum < (method == Method::trapezoidal ? 2e-9 : .0001), "PWL RC analytical transient");
            const double final = value(result, result.samples.back(), id(4)), initial = rc.components.back().initial;
            near(energy, .5 * .001 * (final * final - initial * initial), 6e-7, "PWL RC integrated energy balance");
            std::cout << method_name(method) << (charge ? " charge" : " leakage") << " max error=" << maximum << '\n';
        }
    }
    // PWL switches accept either current direction and retain a physical off leakage path.
    auto p = fixture(Kind::ideal_switch);
    p.components[0].value = -5;
    p.events = {{.0023, id(11), true}, {.0077, id(11), false}};
    auto r = execute(compile(p));
    for (const auto &s : r.samples)
        near(value(r, s, id(11)), -5 / (10 + (s.time >= .0023 && s.time < .0077 ? .2 : 1000)), 1e-12,
             "Exact gated resistance change");
    check(std::count_if(r.samples.begin(), r.samples.end(),
                        [](const auto &s) { return s.time == .0023 || s.time == .0077; }) == 2,
          "Off-grid gate edges");
    // A positive/negative pulse exercises both slopes without changing source topology.
    p = fixture();
    p.profile.step = .00037;
    p.components[0].value = 10;
    p.components[0].source = {Waveform::pulse, -5, 100, 0, .00013, .35, {}};
    r = execute(compile(p));
    for (const auto &s : r.samples) {
        const bool high = s.time >= .00013 && s.time < .00013 + .35 / 100;
        const double expected = high ? (5 - .7 * (1 - .2 / 1000)) / 10.2 : -5. / 1010;
        near(value(r, s, id(11)), expected, 1e-11, "Pulse diode transitions");
    }
    // Parallel PWL diodes have unique shared currents, unlike a loop of ideal shorts.
    p = fixture();
    p.components[1].negative = id(1);
    p.nodes.pop_back();
    p.components.pop_back();
    auto parallel = p.components[1];
    parallel.id = id(13);
    parallel.name = "Parallel diode";
    parallel.semiconductor.ron = .4;
    p.components.push_back(parallel);
    r = execute(compile(p));
    near(value(r, r.samples.back(), id(11)), (5 - .7) / .2 + .7 / 1000, 1e-10, "First diode shares current");
    near(value(r, r.samples.back(), id(13)), (5 - .7) / .4 + .7 / 1000, 1e-10, "Second diode shares current");
    auto ordered = r.samples;
    std::reverse(p.components.begin(), p.components.end());
    std::reverse(p.nodes.begin(), p.nodes.end());
    r = execute(compile(p));
    check(r.samples.size() == ordered.size(), "Repeatable sample count");
    for (size_t i = 0; i < ordered.size(); ++i)
        check(r.samples[i].values == ordered[i].values, "PWL permutation determinism");
    // An off switch is an explicit resistor: it can carry a current source without a floating island.
    p = fixture(Kind::ideal_switch);
    p.nodes.pop_back();
    p.components.pop_back();
    p.components[1].negative = id(1);
    p.components[0].kind = Kind::current;
    p.components[0].value = -.001;
    r = execute(compile(p));
    near(value(r, r.samples.back(), id(2)), 1, 1e-12, "Explicit off-state path");
    p.components[1].semiconductor.model = SemiconductorModel::ideal;
    error("current_cutset", [&] { execute(compile(p)); });
    for (const char *key : {"ron", "roff", "forward_voltage"}) {
        p = fixture();
        auto &field = *semiconductor_parameter(p.components[1].semiconductor, key);
        for (double invalid :
             {-1., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
            field = invalid;
            error("invalid_semiconductor", [&] { compile(p); });
        }
    }
    p = fixture();
    p.components[1].semiconductor.roff = .1;
    error("invalid_semiconductor", [&] { compile(p); });
    p = fixture();
    p.components[0].semiconductor = p.components[1].semiconductor;
    error("invalid_semiconductor", [&] { compile(p); });
    p = fixture();
    Document document(p);
    const auto original = document.root_project();
    document.apply("Edit model", [&](Project &q) { write_property(q, id(11), "ron", .5); });
    check(document.root_project().components[1].semiconductor.ron == .5, "Property binding");
    document.undo();
    check(document.root_project() == original, "Undo complete model");
    document.redo();
    auto instance = document.create_definition({id(11)}, "Device");
    auto definition_id = document.root_project().instances.front().definition;
    document.edit_definition(definition_id, [&](Definition &d) {
        d.parameters.push_back({id(50), "Ron", "Ohm", id(11), "ron", .5});
    });
    document.apply("Override Ron",
                   [&](Project &q) { write_property(q, instance, "parameter/" + id(50), .9); });
    const auto flat = flatten(document.root_project()).project;
    check(std::any_of(flat.components.begin(), flat.components.end(),
                      [](const auto &c) { return c.kind == Kind::diode && c.semiconductor.ron == .9; }),
          "Public PWL parameter flatten");
    roundtrip(document.root_project());
    execute(compile(document.root_project()));
    // v8 files remain ideal; the writer records schema9 even for defaults.
    p = fixture();
    p.components[1].semiconductor = {};
    auto old = encode(p);
    old.replace(0, 15, "PowerDriveSim 8");
    std::istringstream legacy(old);
    check(read_project(legacy) == p, "v8 ideal migration");
    auto invalid_record = [&](const std::string &record, const char *code) {
        std::istringstream input(encode(p) + record + "\n");
        error(code, [&] { read_project(input); });
    };
    invalid_record("semiconductor \"" + id(11) + "\" 2 .1 1000 .7", "parse_error");
    invalid_record("semiconductor \"" + id(11) + "\" 1 0 1000 .7", "invalid_semiconductor");
    invalid_record("semiconductor \"" + id(10) + "\" 1 .1 1000 .7", "invalid_semiconductor");
    invalid_record("semiconductor \"" + id(99) + "\" 1 .1 1000 .7", "missing_semiconductor_target");
    const auto record = "semiconductor \"" + id(11) + "\" 1 .1 1000 .7\n";
    invalid_record(record + record, "parse_error");
    std::istringstream unsupported(old + record);
    error("parse_error", [&] { read_project(unsupported); });
    for (const auto *name : {"diode-freewheel", "vsi-2l", "npc-3l"}) {
        std::ifstream input(std::string(argv[1]) + "/examples/" + name + ".pds");
        auto project = read_project(input);
        auto change = [](auto &body) {
            for (auto &c : body.components)
                if (c.kind == Kind::diode || c.kind == Kind::ideal_switch)
                    c.semiconductor.model = SemiconductorModel::piecewise_linear;
        };
        change(project);
        for (auto &d : project.definitions)
            change(d);
        auto run = execute(compile(project));
        check(!run.cancelled && !run.samples.empty() && run.last_time == project.profile.stop,
              "Converter generic PWL execution");
        check(run.max_scaled_residual < 1e-9, "Converter PWL linear residual");
        roundtrip(project);
        std::cout << name << " PWL steps=" << run.accepted_steps
                  << " max iterations=" << run.max_step_iterations << '\n';
    }
    std::cout << "PASS PWL I-V, polarity, leakage, energy, events, hierarchy and converters\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
