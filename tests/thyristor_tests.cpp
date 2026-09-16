#include "core/editor/document.hpp"
#include "core/editor/properties.hpp"
#include "core/model/connectivity.hpp"
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
    return derived_uuid("thyristor-test/" + std::to_string(n));
}
static void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
static void near(double value, double expected, double tolerance, const char *message) {
    if (!std::isfinite(value) || std::abs(value - expected) > tolerance)
        throw std::runtime_error(std::string(message) + ": " + std::to_string(value) + " vs " +
                                 std::to_string(expected));
}
static void error(const char *code, const std::function<void()> &f) {
    try {
        f();
    } catch (const Diagnostic &e) {
        check(e.code == code, "Wrong diagnostic");
        return;
    }
    throw std::runtime_error(std::string("Missing diagnostic: ") + code);
}
static Project fixture(bool pwl = false) {
    Project p;
    p.id = id(0);
    p.name = "Thyristor";
    p.profile = {.014, .001};
    p.nodes = {{id(1), "Ground", true}, {id(2), "Input", false}, {id(3), "Output", false}};
    p.components = {{id(10), "Source", Kind::voltage, id(2), id(1), 5},
                    {id(11), "Thyristor", Kind::thyristor, id(2), id(3), 0},
                    {id(12), "Load", Kind::resistor, id(3), id(1), 10}};
    if (pwl)
        p.components[1].semiconductor = {SemiconductorModel::piecewise_linear, .2, 1000, .7};
    return p;
}
static double value(const Result &r, const Sample &s, const std::string &object = id(11)) {
    auto c =
        std::find_if(r.channels.begin(), r.channels.end(), [&](const auto &c) { return c.object == object; });
    check(c != r.channels.end(), "Missing result channel");
    return s.values[size_t(c - r.channels.begin())];
}
static std::string encode(const Project &p) {
    std::ostringstream out;
    write_project(p, out);
    return out.str();
}
static Project decode(const std::string &s) {
    std::istringstream in(s);
    return read_project(in);
}
int main(int argc, char **argv) try {
    check(argc == 2, "Pass repository root");
    for (bool pwl : {false, true})
        for (auto method : {Method::backward_euler, Method::trapezoidal}) {
            auto p = fixture(pwl);
            p.profile.method = method;
            const double on = pwl ? (5 - .7 * (1 - .2 / 1000)) / 10.2 : .5;
            const double off = pwl ? 5. / 1010 : 0;
            auto r = execute(compile(p));
            for (const auto &s : r.samples)
                near(value(r, s), off, 1e-12, "Ungated forward blocking");
            p.components[1].semiconductor.initial_latched = true;
            r = execute(compile(p));
            for (const auto &s : r.samples)
                near(value(r, s), on, 1e-12, "Initial latch");
            p.components[1].semiconductor.initial_latched = false;
            p.components[0].value = 10;
            p.components[0].source = {Waveform::pulse, -5, 1 / .012, 0, 0, .5};
            p.events = {
                {.0023, id(11), true}, {.0027, id(11), false}, {.0131, id(11), true}, {.0134, id(11), false}};
            r = execute(compile(p));
            check(r.gate_objects == std::vector<std::string>{id(11)}, "Thyristor gate channel");
            for (const auto &s : r.samples) {
                const bool reverse = s.time >= .006 && s.time < .012;
                const bool conducting = (s.time >= .0023 && s.time < .006) || s.time >= .0131;
                near(value(r, s), conducting ? on : (reverse ? -off : off), 1e-11,
                     "Fire, hold, commutate and rearm");
                check(s.gates[0] ==
                          ((s.time >= .0023 && s.time < .0027) || (s.time >= .0131 && s.time < .0134)),
                      "Gate records command, not latch");
                const double voltage = value(r, s, id(2)), load = value(r, s, id(3));
                near(voltage * value(r, s, id(10)) + load * load / 10 + (voltage - load) * value(r, s), 0,
                     1e-11, "Power balance");
            }
            for (const auto &e : p.events)
                check(std::any_of(r.samples.begin(), r.samples.end(),
                                  [&](const auto &s) { return s.time == e.time; }),
                      "Off-grid gate edge");
            check(decode(encode(p)) == p, "Thyristor serialization");
            std::reverse(p.components.begin(), p.components.end());
            std::reverse(p.nodes.begin(), p.nodes.end());
            auto reordered = execute(compile(p));
            check(r.samples.size() == reordered.samples.size(), "Stable sample count");
            for (size_t i = 0; i < r.samples.size(); ++i)
                check(r.samples[i].values == reordered.samples[i].values &&
                          r.samples[i].gates == reordered.samples[i].gates,
                      "Stable UUID ordering");

            p = fixture(pwl);
            p.profile.method = method;
            p.components[0].value = -5;
            p.components[1].closed = true;
            p.components[1].semiconductor.initial_latched = true;
            r = execute(compile(p));
            for (const auto &s : r.samples)
                near(value(r, s), -off, 1e-12, "Gate cannot force reverse conduction");
        }
    // Current falls below Ih while the anode is still forward biased; restoring voltage does not retrigger.
    for (bool pwl : {false, true}) {
        auto p = fixture(pwl);
        p.profile = {.02, .0001};
        auto &t = p.components[1];
        t.semiconductor.holding_current = .1;
        t.semiconductor.initial_latched = true;
        p.components[0].source.kind = Waveform::piecewise_linear;
        p.components[0].source.points = {{0, 5}, {.01, 0}, {.02, 5}};
        const auto r = execute(compile(p));
        bool released = false;
        for (const auto &s : r.samples) {
            const double voltage = s.time <= .01 ? 5 - 500 * s.time : 500 * (s.time - .01);
            const double on = pwl ? (voltage - .7 * (1 - .2 / 1000)) / 10.2 : voltage / 10;
            if (on < .1 - 1e-9)
                released = true;
            near(value(r, s), released ? (pwl ? voltage / 1010 : 0) : on, 1e-10,
                 "Holding threshold and memory");
        }
        check(released, "Holding test must cross threshold");
    }
    // A gate shorter than the time required to reach Ih must not latch an inductive load.
    for (auto method : {Method::backward_euler, Method::trapezoidal}) {
        auto p = fixture(true);
        p.profile = {.001, 1e-6, method};
        p.nodes.push_back({id(4), "Inductor", false});
        p.components[2].negative = id(4);
        p.components.push_back({id(13), "L", Kind::inductor, id(4), id(1), .1});
        p.components[1].semiconductor.holding_current = .1;
        p.events = {{0, id(11), true}, {.0001, id(11), false}};
        auto r = execute(compile(p));
        for (const auto &s : r.samples)
            if (s.time >= .0001) {
                const double voltage = value(r, s, id(2)) - value(r, s, id(3));
                near(value(r, s), voltage / 1000, 1e-12, "Short gate returns to blocking path");
            }
    }
    // An ideal ungated thyristor must not be activated by the singular-system fallback.
    auto p = fixture();
    p.components[0].kind = Kind::current;
    p.components[0].value = -1;
    error("nonlinear_convergence", [&] { execute(compile(p)); });
    p = fixture(true);
    p.components[1].semiconductor.holding_current = .03;
    auto serialized = encode(p);
    auto old = serialized;
    old.replace(0, old.find('\n'), "PowerDriveSim 10");
    const auto initialization = old.find("initialization ");
    old.erase(initialization, old.find('\n', initialization) - initialization + 1);
    error("schema_version", [&] { decode(old); });
    error("parse_error", [&] { decode(serialized + "thyristor \"" + id(11) + "\" 0.03 0\n"); });
    error("invalid_thyristor", [&] { decode(serialized + "thyristor \"" + id(10) + "\" 0.03 0\n"); });
    for (double invalid :
         {-1., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        auto bad = p;
        bad.components[1].semiconductor.holding_current = invalid;
        error("invalid_thyristor", [&] { compile(bad); });
    }
    auto bad = p;
    bad.components[1].kind = Kind::diode;
    error("invalid_thyristor", [&] { compile(bad); });
    Document doc(p);
    const auto original = doc.root_project();
    doc.apply("Holding", [&](Project &q) {
        write_property(q, id(11), "holding_current", .05);
        write_property(q, id(11), "initial_latched", true);
    });
    doc.undo();
    check(doc.root_project() == original, "Latch property undo");
    doc.redo();
    const auto instance = doc.create_definition({id(11)}, "Thyristor");
    const auto def = doc.root_project().instances.front().definition;
    doc.edit_definition(def, [&](Definition &d) {
        d.parameters.push_back({id(50), "Ih", "A", id(11), "holding_current", .05});
    });
    doc.apply("Instance Ih", [&](Project &q) { write_property(q, instance, "parameter/" + id(50), .08); });
    auto flat = flatten(doc.root_project()).project;
    check(std::any_of(flat.components.begin(), flat.components.end(),
                      [](const auto &c) {
                          return c.kind == Kind::thyristor && c.semiconductor.holding_current == .08 &&
                                 c.semiconductor.initial_latched;
                      }),
          "Latch flatten and parameter");
    check(decode(encode(doc.root_project())) == doc.root_project(), "Hierarchy serialization");
    execute(compile(doc.root_project()));
    std::ifstream example(std::string(argv[1]) + "/examples/thyristor-halfwave.pds");
    p = read_project(example);
    const auto device = std::find_if(p.components.begin(), p.components.end(),
                                     [](const auto &c) { return c.kind == Kind::thyristor; });
    check(device != p.components.end(), "Shipped thyristor example");
    for (auto method : {Method::backward_euler, Method::trapezoidal}) {
        p.profile.method = method;
        auto r = execute(compile(p));
        double integral = 0;
        for (size_t i = 0; i < r.samples.size(); ++i) {
            const auto &s = r.samples[i];
            const bool on = (s.time >= .0025 && s.time < .01) || (s.time >= .0225 && s.time < .03);
            const double expected = on ? std::sin(2 * std::acos(-1.) * 50 * s.time) : 0;
            near(value(r, s, device->id), expected, 1e-10, "Controlled half-wave analytical current");
            if (i)
                integral += (s.time - r.samples[i - 1].time) *
                            (value(r, s, device->id) + value(r, r.samples[i - 1], device->id)) / 2;
        }
        const double average = integral / p.profile.stop;
        near(average, (1 + std::cos(std::acos(-1.) / 4)) / (2 * std::acos(-1.)), .0002,
             "Controlled rectifier mean current");
        std::cout << "Half-wave mean current: " << average << " A\n";
    }
    std::cout << "PASS thyristor firing, hold, current threshold, reverse blocking, events, energy and "
                 "persistence\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
