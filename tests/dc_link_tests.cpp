#include "core/editor/document.hpp"
#include "core/model/hierarchy.hpp"
#include "core/solver/reference/reference.hpp"
#include "formats/project/project.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
using namespace pds;
static void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
static void near(double a, double b, double tol, const char *message) {
    if (!std::isfinite(a) || std::abs(a - b) > tol)
        throw std::runtime_error(std::string(message) + ": " + std::to_string(a) + " vs " +
                                 std::to_string(b));
}
static double charge_voltage(double t) {
    double v = 48 * (1 - std::exp(-std::min(t, .05) / .0101));
    if (t > .05)
        v = 48 + (v - 48) * std::exp(-std::min(t - .05, .02) / .0001);
    if (t > .08)
        v *= std::exp(-(t - .08) / .1001);
    return v;
}
static bool braking_on(double t) {
    return (t >= .01 && t < .03) || t >= .04;
}
static double braking_voltage(double t) {
    double v = 48 + 500 * std::min(t, .01);
    if (t > .01)
        v = 20 + (v - 20) * std::exp(-std::min(t - .01, .02) / .0401);
    if (t > .03)
        v += 500 * std::min(t - .03, .01);
    if (t > .04)
        v = 20 + (v - 20) * std::exp(-(t - .04) / .0401);
    return v;
}
static void verify(const Project &p, bool braking) {
    const auto r = execute(compile(p));
    const auto circuit = resolve_connections(p).project;
    std::map<std::string, size_t> indices;
    for (size_t k = 0; k < r.channels.size(); ++k) {
        indices[r.channels[k].object] = k;
    }
    const auto bus_component =
        std::find_if(circuit.components.begin(), circuit.components.end(),
                     [&](const auto &c) { return c.name == (braking ? "Regenerated current" : "Iin"); });
    check(bus_component != circuit.components.end(), "DC bus measurement terminal");
    const auto bus = indices.at(bus_component->negative);
    const auto capacitor = std::find_if(circuit.components.begin(), circuit.components.end(),
                                        [](const auto &c) { return c.kind == Kind::capacitor; });
    check(capacitor != circuit.components.end(), "DC-link capacitor");
    auto vc = [&](const Sample &s) {
        return s.values[indices.at(capacitor->positive)] - s.values[indices.at(capacitor->negative)];
    };
    double max_voltage_error = 0, work = 0, numerical_loss = 0;
    for (size_t k = 0; k < r.samples.size(); ++k) {
        const auto &s = r.samples[k];
        const double expected = braking ? braking_voltage(s.time) : charge_voltage(s.time);
        const double output = braking        ? (expected + .05) / (braking_on(s.time) ? 1.0025 : 1)
                              : s.time < .05 ? (10 * expected + 4.8) / 10.1
                              : s.time < .07 ? 48
                              : s.time < .08 ? expected
                                             : expected / 1.001;
        max_voltage_error = std::max(max_voltage_error, std::abs(vc(s) - expected));
        near(s.values[bus], output, p.profile.method == Method::backward_euler ? .003 : .00002,
             "Terminal DC-link voltage");
        double power = 0;
        for (const auto &c : circuit.components) {
            const double v = s.values[indices.at(c.positive)] - s.values[indices.at(c.negative)];
            power += v * s.values[indices.at(c.id)];
        }
        near(power, 0, 1e-7, "DC-link instantaneous power balance");
        if (!k)
            continue;
        const auto &a = r.samples[k - 1];
        const bool be = p.profile.method == Method::backward_euler;
        const double v = be ? vc(s) : (vc(a) + vc(s)) / 2;
        double net_power = 0;
        if (braking) {
            const bool on = braking_on(a.time);
            const double terminal = (v + .05) / (on ? 1.0025 : 1);
            const double resistor_current = on ? terminal / 40 : 0;
            const double cap_current = .5 - resistor_current;
            net_power =
                .5 * terminal - 40 * resistor_current * resistor_current - .1 * cap_current * cap_current;
        } else if (a.time < .07) {
            const double resistance = a.time < .05 ? 10.1 : .1;
            const double current = (48 - v) / resistance;
            net_power = 48 * current - resistance * current * current;
        } else if (a.time >= .08)
            net_power = -v * v / 100.1;
        work += (s.time - a.time) * net_power;
        if (be)
            numerical_loss += .0005 * std::pow(vc(s) - vc(a), 2);
    }
    near(max_voltage_error, 0, p.profile.method == Method::backward_euler ? .003 : .00002,
         "Analytical capacitor voltage");
    near(work,
         .0005 * (std::pow(vc(r.samples.back()), 2) - std::pow(vc(r.samples.front()), 2)) + numerical_loss,
         1e-8, "DC-link source work, dissipation and stored energy");
}
int main(int argc, char **argv) try {
    check(argc == 2, "Pass repository root");
    for (bool braking : {false, true}) {
        std::ifstream input(std::string(argv[1]) + "/examples/" +
                            (braking ? "braking-chopper" : "precharge-discharge") + ".pds");
        auto p = read_project(input);
        for (auto method : {Method::backward_euler, Method::trapezoidal}) {
            p.profile.method = method;
            verify(p, braking);
            std::ostringstream out;
            write_project(p, out);
            std::istringstream in(out.str());
            check(read_project(in) == p, "Auxiliary power circuit roundtrip");
            Document document(p);
            for (const auto &instance : p.instances)
                document.expand_instance(instance.id);
            verify(document.root_project(), braking);
        }
    }
    std::cout << "PASS precharge, bypass, isolation, discharge and braking RC response and energy\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
