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
static void near(double a, double b, double tolerance, const char *message) {
    if (!std::isfinite(a) || std::abs(a - b) > tolerance)
        throw std::runtime_error(std::string(message) + ": " + std::to_string(a) + " vs " +
                                 std::to_string(b));
}
static size_t channel(const Result &r, const std::string &name) {
    for (size_t i = 0; i < r.channels.size(); ++i)
        if (r.channels[i].name == name)
            return i;
    throw std::runtime_error("Missing " + name);
}
static double average(const Result &r, size_t channel_index, double begin = .04) {
    double integral = 0, duration = 0;
    for (size_t k = 1; k < r.samples.size(); ++k) {
        const auto &a = r.samples[k - 1], &b = r.samples[k];
        if (a.time < begin)
            continue;
        const auto dt = b.time - a.time;
        integral += dt * (a.values[channel_index] + b.values[channel_index]) / 2;
        duration += dt;
    }
    return integral / duration;
}
static void verify(const Project &p, const std::string &kind) {
    const auto r = execute(compile(p));
    const auto u = channel(r, "u:Uout"), current = channel(r, "i:Iout");
    const double expected = kind == "buck" ? 9.6 : kind == "boost" ? 40 : -16;
    near(average(r, u), expected, std::abs(expected) * .006,
         "CCM conversion ratio with explicit capacitor ESR");
    near(average(r, current), average(r, u) / 20, 1e-9, "Output current");
    const auto circuit = resolve_connections(p).project;
    std::map<std::string, size_t> channels;
    for (size_t k = 0; k < r.channels.size(); ++k)
        channels[r.channels[k].object] = k;
    const Component *inductor = nullptr, *capacitor = nullptr;
    for (const auto &c : circuit.components) {
        if (c.kind == Kind::inductor)
            inductor = &c;
        if (c.kind == Kind::capacitor)
            capacitor = &c;
    }
    check(inductor && capacitor, "LC storage");
    auto il = [&](const Sample &s) { return s.values[channels.at(inductor->id)]; };
    auto vc = [&](const Sample &s) {
        return s.values[channels.at(capacitor->positive)] - s.values[channels.at(capacitor->negative)];
    };
    // Independent terminal-power equations for these three circuits. The gate
    // from the interval's start is also used at its end, before a PWM edge.
    auto input_minus_loss = [&](double i, double v, bool on) {
        const double injected = kind == "buck" ? i : on ? 0 : kind == "boost" ? i : -i;
        const double ic = (20 * injected - v) / 20.05;
        const double out = v + .05 * ic;
        const double supply_current = kind == "boost" ? i : on ? i : 0;
        return 24 * supply_current - out * out / 20 - .05 * ic * ic;
    };
    double work = 0, numerical_loss = 0;
    for (const auto &s : r.samples) {
        double total = 0;
        for (const auto &c : circuit.components) {
            const double v = s.values[channels.at(c.positive)] - s.values[channels.at(c.negative)];
            const double i = s.values[channels.at(c.id)], power = v * i;
            total += power;
            if (c.kind == Kind::diode)
                check(v < 1e-7 && i > -1e-7 && std::abs(power) < 1e-6, "DC/DC diode complementarity");
            if (c.kind == Kind::resistor)
                check(power >= -1e-9, "Resistive loss");
            if (c.kind == Kind::ideal_switch)
                near(power, 0, 1e-6, "Ideal switch power");
        }
        near(total, 0, 1e-6, "Instantaneous DC/DC power balance");
    }
    for (size_t k = 1; k < r.samples.size(); ++k) {
        const auto &a = r.samples[k - 1], &b = r.samples[k];
        const bool be = p.profile.method == Method::backward_euler;
        const double i = be ? il(b) : (il(a) + il(b)) / 2, v = be ? vc(b) : (vc(a) + vc(b)) / 2;
        work += (b.time - a.time) * input_minus_loss(i, v, a.gates.front());
        if (be)
            numerical_loss += .5 * inductor->value * std::pow(il(b) - il(a), 2) +
                              .5 * capacitor->value * std::pow(vc(b) - vc(a), 2);
    }
    auto energy = [&](const Sample &s) {
        return .5 * inductor->value * il(s) * il(s) + .5 * capacitor->value * vc(s) * vc(s);
    };
    const double energy_change = energy(r.samples.back()) - energy(r.samples.front());
    near(work, energy_change + numerical_loss, 1e-6, "Source work, load/ESR loss and stored energy");
    std::cout << kind << " " << method_name(p.profile.method) << ": Uavg=" << average(r, u)
              << ", energy error=" << work - energy_change - numerical_loss << '\n';
}
static void bidirectional(const Project &p, bool discharge) {
    const auto r = execute(compile(p));
    const auto i = channel(r, "i:Ilow"), high = channel(r, "i:HV supply"), low = channel(r, "i:LV source");
    const double battery = discharge ? 13.5 : 10.5;
    double exact = 0, error = 0, work = 0, numerical_loss = 0;
    for (size_t k = 1; k < r.samples.size(); ++k) {
        const auto &a = r.samples[k - 1], &b = r.samples[k];
        const bool on = int(std::floor(a.time * 20000 + 1e-9)) % 2 == 0;
        const double source = on ? 24 : 0, dt = b.time - a.time;
        const double target = (source - battery) / .5;
        exact = target + (exact - target) * std::exp(-dt / .004);
        error = std::max(error, std::abs(b.values[i] - exact));
        const bool be = p.profile.method == Method::backward_euler;
        const double current = be ? b.values[i] : (a.values[i] + b.values[i]) / 2;
        work += dt * ((source - battery) * current - .5 * current * current);
        if (be)
            numerical_loss += .001 * std::pow(b.values[i] - a.values[i], 2);
    }
    near(error, 0, p.profile.method == Method::backward_euler ? .0003 : 1e-7,
         "Bidirectional analytical RL current");
    near(average(r, i, .03), discharge ? -3 : 3, .003, "Bidirectional mean current");
    check((average(r, high, .03) > 0) == discharge, "HV source absorbs energy on return");
    check((average(r, low, .03) < 0) == discharge, "LV source delivers energy on return");
    near(work, .001 * std::pow(r.samples.back().values[i], 2) + numerical_loss, 1e-8,
         "Bidirectional energy balance");
}
int main(int argc, char **argv) try {
    check(argc == 2, "Pass repository root");
    for (const std::string kind : {"buck", "boost", "buck-boost"}) {
        std::ifstream input(std::string(argv[1]) + "/examples/" + kind + ".pds");
        auto p = read_project(input);
        check(p.definitions.size() == 1 && p.definitions[0].components.size() == 5,
              "Five editable power-stage atoms");
        check(p.definitions[0].parameters.size() == 5, "Exposed L/C/ESR and initial states");
        for (auto method : {Method::backward_euler, Method::trapezoidal}) {
            p.profile.method = method;
            verify(p, kind);
            std::ostringstream out;
            write_project(p, out);
            std::istringstream in(out.str());
            check(read_project(in) == p, "DC/DC roundtrip");
            Document document(p);
            document.expand_instance(p.instances[0].id);
            verify(document.root_project(), kind);
            document.undo();
            check(document.root_project() == p, "DC/DC expansion undo");
        }
    }
    for (bool discharge : {false, true}) {
        std::ifstream input(std::string(argv[1]) + "/examples/bidirectional-" +
                            (discharge ? "discharge" : "charge") + ".pds");
        auto p = read_project(input);
        check(p.definitions.size() == 2 && p.definitions[0].instances.size() == 1,
              "Bidirectional stage reuses half bridge");
        for (auto method : {Method::backward_euler, Method::trapezoidal}) {
            p.profile.method = method;
            bidirectional(p, discharge);
            Document document(p);
            document.expand_instance(p.instances[0].id);
            bidirectional(document.root_project(), discharge);
            document.undo();
            check(document.root_project() == p, "Bidirectional expansion undo");
            std::ostringstream out;
            write_project(p, out);
            std::istringstream in(out.str());
            check(read_project(in) == p, "Bidirectional recorded gates roundtrip");
        }
    }
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
