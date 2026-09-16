#include "core/editor/document.hpp"
#include "core/model/hierarchy.hpp"
#include "core/solver/reference/reference.hpp"
#include "formats/project/project.hpp"
#include <algorithm>
#include <array>
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
static constexpr int states[6][3] = {{1, 0, -1}, {0, 1, -1}, {-1, 1, 0}, {-1, 0, 1}, {0, -1, 1}, {1, -1, 0}};
static double exact(double time, size_t phase) {
    double value = 0;
    for (size_t k = 0; k < 6; ++k) {
        const double dt = std::clamp(time - k * .001, 0., .001);
        const double target = 2.4 * states[k][phase];
        value = target + (value - target) * std::exp(-dt / .001);
    }
    return value;
}
static void verify(const Project &p) {
    const auto r = execute(compile(p));
    const auto circuit = resolve_connections(p).project;
    std::map<std::string, size_t> indices, names;
    for (size_t k = 0; k < r.channels.size(); ++k) {
        indices[r.channels[k].object] = k;
        names[r.channels[k].name] = k;
    }
    const bool be = p.profile.method == Method::backward_euler;
    double work = 0, loss = 0, stored = 0, damping = 0;
    for (size_t k = 0; k < r.samples.size(); ++k) {
        const auto &s = r.samples[k];
        const auto interval = std::min(size_t(5), size_t((s.time + 1e-12) / .001));
        double power = 0;
        for (const auto &c : circuit.components)
            if (c.kind != Kind::voltage_probe)
                power += (s.values.at(indices.at(c.positive)) - s.values.at(indices.at(c.negative))) *
                         s.values.at(indices.at(c.id));
        near(power, 0, 1e-7, "Dual inverter power balance");
        stored = 0;
        for (size_t phase = 0; phase < 3; ++phase) {
            const auto suffix = std::string(1, char('A' + phase));
            const auto u = names.at("u:U" + suffix), i = names.at("i:I" + suffix);
            near(s.values[u], 24 * states[interval][phase], 1e-8, "Independent winding voltage");
            near(s.values[i], exact(s.time, phase), be ? .002 : 2e-6, "Open-end winding RL response");
            stored += .005 * s.values[i] * s.values[i];
            if (k) {
                const auto &prior = r.samples[k - 1];
                const auto previous_interval = std::min(size_t(5), size_t((prior.time + 1e-12) / .001));
                const double current = be ? s.values[i] : (s.values[i] + prior.values[i]) / 2;
                const double dt = s.time - prior.time;
                work += dt * 24 * states[previous_interval][phase] * current;
                loss += dt * 10 * current * current;
                if (be)
                    damping += .005 * std::pow(s.values[i] - prior.values[i], 2);
            }
        }
    }
    near(work, stored + loss + damping, 1e-8, "Supplied work, copper loss and winding energy");
}
int main(int argc, char **argv) try {
    check(argc == 2, "Pass repository root");
    std::ifstream input(std::string(argv[1]) + "/examples/open-end-winding.pds");
    auto p = read_project(input);
    const auto flat = flatten(p).project;
    check(std::count_if(flat.components.begin(), flat.components.end(),
                        [](const auto &c) { return c.kind == Kind::ideal_switch; }) == 12,
          "Six independent half bridges");
    for (auto method : {Method::backward_euler, Method::trapezoidal}) {
        p.profile.method = method;
        verify(p);
        Document document(p);
        document.expand_instance(p.instances.front().id);
        verify(document.root_project());
        document.undo();
        check(document.root_project() == p, "Open-end expansion undo");
        std::ostringstream output;
        write_project(p, output);
        std::istringstream saved(output.str());
        check(read_project(saved) == p, "Open-end converter roundtrip");
        auto permuted = p;
        std::reverse(permuted.definitions.begin(), permuted.definitions.end());
        for (auto &d : permuted.definitions) {
            std::reverse(d.components.begin(), d.components.end());
            std::reverse(d.events.begin(), d.events.end());
        }
        const auto a = execute(compile(p)), b = execute(compile(permuted));
        check(a.samples.size() == b.samples.size(), "Stable step count");
        for (size_t k = 0; k < a.samples.size(); ++k)
            check(a.samples[k].values == b.samples[k].values, "Stable six-leg commutation");
    }
    std::cout << "PASS open-end winding levels, RL response, power, energy and expansion\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
