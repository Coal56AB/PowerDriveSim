#include "core/editor/document.hpp"
#include "core/model/hierarchy.hpp"
#include "core/model/waveform.hpp"
#include "core/solver/reference/reference.hpp"
#include "formats/project/project.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <numbers>
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
static void verify(const Project &p, bool three, bool pwl) {
    const auto r = execute(compile(p));
    const auto u = channel(r, "u:Udc"), i = channel(r, "i:Idc");
    const auto circuit = resolve_connections(p).project;
    std::map<std::string, size_t> channels;
    for (size_t k = 0; k < r.channels.size(); ++k)
        channels[r.channels[k].object] = k;
    double integral = 0, previous = 0, previous_time = 0;
    for (const auto &s : r.samples) {
        double low = 0, high = 0;
        bool first = three;
        for (const auto &c : circuit.components)
            if (c.kind == Kind::voltage) {
                const auto v = source_value(c, s.time);
                if (first) {
                    low = high = v;
                    first = false;
                } else {
                    low = std::min(low, v);
                    high = std::max(high, v);
                }
            }
        const double expected = high - low;
        if (!pwl)
            near(s.values[u], expected, 1e-8, "Ideal rectified voltage");
        else
            check(s.values[u] >= -1e-8 && s.values[u] <= expected + 1e-8 && expected - s.values[u] < 1.8,
                  "PWL bridge drop and passivity");
        near(s.values[i], s.values[u] / 10, 1e-8, "Load current");
        double power = 0;
        for (const auto &c : circuit.components) {
            const double v = s.values[channels.at(c.positive)] - s.values[channels.at(c.negative)];
            if (c.kind == Kind::voltage_probe)
                continue;
            const double current = s.values[channels.at(c.id)];
            power += v * current;
            if (c.kind == Kind::diode) {
                check(v * current >= -1e-7, "Diode passivity");
                if (!pwl)
                    check(v < 1e-7 && current > -1e-7 && std::abs(v * current) < 1e-6,
                          "Diode complementarity");
            }
        }
        near(power, 0, 1e-5, "Instantaneous power balance");
        integral += (s.time - previous_time) * (previous + s.values[u]) / 2;
        previous = s.values[u];
        previous_time = s.time;
    }
    if (!pwl)
        near(integral / p.profile.stop, (three ? 3 * std::sqrt(3.) : 2) * 100 / std::numbers::pi, .001,
             "Mean rectified voltage");
}
int main(int argc, char **argv) try {
    check(argc == 2, "Pass repository root");
    for (bool three : {false, true}) {
        const std::string key = three ? "diode-bridge-3p" : "diode-bridge-1p";
        std::ifstream input(std::string(argv[1]) + "/examples/" + key + ".pds");
        auto p = read_project(input);
        check(p.definitions.size() == 1 && p.definitions[0].components.size() == (three ? 6u : 4u),
              "Editable atomic diode bridge");
        for (bool pwl : {false, true})
            for (auto method : {Method::backward_euler, Method::trapezoidal}) {
                p.profile.method = method;
                for (auto &d : p.definitions[0].components)
                    d.semiconductor.model =
                        pwl ? SemiconductorModel::piecewise_linear : SemiconductorModel::ideal;
                verify(p, three, pwl);
                std::ostringstream out;
                write_project(p, out);
                std::istringstream in(out.str());
                check(read_project(in) == p, "Bridge roundtrip");
                Document document(p);
                document.expand_instance(p.instances[0].id);
                verify(document.root_project(), three, pwl);
                document.undo();
                check(document.root_project() == p, "Bridge expansion undo");
                auto reversed = p;
                std::reverse(reversed.definitions[0].components.begin(),
                             reversed.definitions[0].components.end());
                std::reverse(reversed.wires.begin(), reversed.wires.end());
                const auto a = execute(compile(p)), b = execute(compile(reversed));
                check(a.samples.size() == b.samples.size(), "Deterministic sample count");
                for (size_t k = 0; k < a.samples.size(); ++k)
                    check(a.samples[k].values == b.samples[k].values, "Bridge deterministic ordering");
            }
    }
    std::cout << "PASS single/three-phase ideal/PWL bridges, waveform, mean, KCL, power, persistence and "
                 "expansion\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
