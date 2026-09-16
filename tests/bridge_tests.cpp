#include "core/editor/document.hpp"
#include "core/model/hierarchy.hpp"
#include "core/solver/reference/reference.hpp"
#include "formats/project/project.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
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
static size_t channel(const Result &r, const std::string &name) {
    for (size_t i = 0; i < r.channels.size(); ++i)
        if (r.channels[i].name == name)
            return i;
    throw std::runtime_error("Missing " + name);
}
static double voltage(double time, double amplitude) {
    return (int(std::floor(time / .001 + 1e-10)) % 2 ? -1 : 1) * amplitude;
}
static double exact_current(double time, double amplitude) {
    double current = 0;
    for (int k = 0; k < 6; ++k) {
        const double dt = std::clamp(time - k * .001, 0., .001);
        const double target = (k % 2 ? -1 : 1) * amplitude / 10;
        current = target + (current - target) * std::exp(-dt / .001);
    }
    return current;
}
static void verify(const Project &p, bool full, bool deadtime) {
    const auto r = execute(compile(p));
    const auto u = channel(r, "u:Uload"), i = channel(r, "i:Iload");
    const double amplitude = full ? 24 : 12;
    double error = 0, work = 0, numerical_loss = 0;
    bool diode_conducted = false;
    for (size_t k = 0; k < r.samples.size(); ++k) {
        const auto &s = r.samples[k];
        near(s.values[u], voltage(s.time, amplitude), 1e-8, "Bridge voltage and freewheel polarity");
        error = std::max(error, std::abs(s.values[i] - exact_current(s.time, amplitude)));
        for (size_t c = 0; c < r.channels.size(); ++c)
            if (r.channels[c].name.find("/DH") != std::string::npos ||
                r.channels[c].name.find("/DL") != std::string::npos)
                if (r.channels[c].name.rfind("i:", 0) == 0 && s.values[c] > .01)
                    diode_conducted = true;
        if (!k)
            continue;
        const auto &a = r.samples[k - 1];
        const bool be = p.profile.method == Method::backward_euler;
        const double current = be ? s.values[i] : (s.values[i] + a.values[i]) / 2;
        work += (s.time - a.time) * (voltage(a.time, amplitude) * current - 10 * current * current);
        if (be)
            numerical_loss += .005 * std::pow(s.values[i] - a.values[i], 2);
    }
    near(error, 0, p.profile.method == Method::backward_euler ? .00005 * amplitude : .000002,
         "Analytical RL current");
    near(work, .005 * std::pow(r.samples.back().values[i], 2) + numerical_loss, 1e-8,
         "RL bridge energy balance");
    if (deadtime)
        check(diode_conducted, "Antiparallel diodes carry dead-time current");
}
int main(int argc, char **argv) try {
    check(argc == 2, "Pass repository root");
    for (bool full : {false, true}) {
        std::ifstream input(std::string(argv[1]) + "/examples/" + (full ? "full" : "half") + "-bridge.pds");
        auto p = read_project(input);
        for (bool deadtime : {false, true})
            for (auto method : {Method::backward_euler, Method::trapezoidal}) {
                auto q = p;
                q.profile.method = method;
                if (deadtime)
                    for (auto &e : q.events)
                        if (e.closed)
                            e.time += .00001;
                q.events.erase(std::remove_if(q.events.begin(), q.events.end(),
                                              [&](const auto &e) { return e.time > q.profile.stop; }),
                               q.events.end());
                verify(q, full, deadtime);
                std::ostringstream out;
                write_project(q, out);
                std::istringstream in(out.str());
                check(read_project(in) == q, "Bridge persistence");
                Document document(q);
                document.expand_instance(q.instances[0].id);
                verify(document.root_project(), full, deadtime);
                document.undo();
                check(document.root_project() == q, "Bridge expansion undo");
            }
        p.patterns[0].initial = p.patterns[1].initial = true;
        try {
            execute(compile(p));
            check(false, "Shoot-through must be diagnosed");
        } catch (const Diagnostic &e) {
            check(e.code == "conflicting_voltage_constraints" ||
                      (e.code == "nonlinear_convergence" &&
                       std::string(e.what()).find("conflicting_voltage_constraints") != std::string::npos),
                  "Shoot-through voltage constraint diagnostic");
        }
    }
    std::cout
        << "PASS half/full bridges, analytical RL, dead-time diodes, energy, persistence and shoot-through\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
