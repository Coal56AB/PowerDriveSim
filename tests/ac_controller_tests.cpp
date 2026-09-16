#include "core/editor/document.hpp"
#include "core/model/hierarchy.hpp"
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
static void verify(const Project &p, bool ideal, double delay, bool enabled = true) {
    const auto result = execute(compile(p));
    const auto resolved = resolve_connections(p).project;
    std::map<std::string, size_t> indices;
    size_t output = 0, current = 0;
    for (size_t k = 0; k < result.channels.size(); ++k) {
        const auto &c = result.channels[k];
        indices[c.object] = k;
        if (c.name == "u:Uload")
            output = k;
        if (c.name == "i:Iload")
            current = k;
    }
    check(result.channels[output].name == "u:Uload" && result.channels[current].name == "i:Iload",
          "AC controller measurement channels");
    const double pi = std::numbers::pi, alpha = delay * 100 * pi;
    const double a = ideal ? 100 : 100 * 10 * (100 + 1e-6) / (1 + 10 * (100 + 1e-6));
    const double b = ideal ? 0 : 10 * .7 * (100 - 1e-6) / (1 + 10 * (100 + 1e-6));
    const double leakage = ideal ? 0 : 100 * 20e-6 / (1 + 20e-6);
    double integral = 0;
    bool held = false;
    for (size_t k = 0; k < result.samples.size(); ++k) {
        const auto &s = result.samples[k];
        const double angle = 100 * pi * s.time;
        const double sine = std::sin(angle), magnitude = std::abs(sine);
        const double phase = std::fmod(angle + 1e-11, pi);
        const bool fired = enabled && phase >= alpha;
        const double expected = std::copysign(
            fired ? std::max(leakage * magnitude, a * magnitude - b) : leakage * magnitude, sine);
        near(s.values[output], expected, 1e-7, "AC phase control and natural extinction");
        near(s.values[current], expected / 10, 1e-8, "AC load current");
        double power = 0;
        for (const auto &c : resolved.components) {
            const double voltage = s.values.at(indices.at(c.positive)) - s.values.at(indices.at(c.negative));
            const double device_power = voltage * s.values.at(indices.at(c.id));
            power += device_power;
            if (c.kind != Kind::voltage)
                check(device_power >= -1e-7, "Passive power circuit");
        }
        near(power, 0, 1e-7, "AC controller instantaneous power balance");
        if (fired && phase > alpha + .1 && phase < pi - .1 && std::abs(expected) > 1) {
            check(std::none_of(s.gates.begin(), s.gates.end(), [](auto gate) { return gate != 0; }),
                  "Short firing pulse has ended");
            held = true;
        }
        if (k) {
            const auto &prior = result.samples[k - 1];
            // The stored sample is right-continuous at a gate edge. Integrate
            // the interval ending there with its left-hand blocking voltage.
            const double before_edge = enabled && std::abs(phase - alpha) < 1e-9
                                           ? leakage * sine
                                           : s.values[output];
            integral += (s.time - prior.time) *
                        (before_edge * before_edge + prior.values[output] * prior.values[output]) /
                        2;
        }
    }
    check(!enabled || held, "Thyristor holds after gate ends");
    const double end = pi - (ideal ? 0 : std::asin(.7 * (1 + 20e-6) / 100));
    const double sin2 = (end - alpha) / 2 - (std::sin(2 * end) - std::sin(2 * alpha)) / 4;
    const double sin1 = std::cos(alpha) - std::cos(end);
    const double rms2 =
        leakage * leakage / 2 +
        (enabled ? ((a * a - leakage * leakage) * sin2 - 2 * a * b * sin1 + b * b * (end - alpha)) / pi : 0);
    near(std::sqrt(integral / p.profile.stop), std::sqrt(rms2), .0001, "Analytical RMS output voltage");
}
int main(int argc, char **argv) try {
    check(argc == 2, "Pass repository root");
    std::ifstream input(std::string(argv[1]) + "/examples/ac-voltage-controller.pds");
    auto p = read_project(input);
    check(p.definitions.size() == 1 && p.definitions[0].components.size() == 2 &&
              p.definitions[0].ports.size() == 4,
          "Two atomic thyristors and separate gates");
    for (bool ideal : {true, false})
        for (auto method : {Method::backward_euler, Method::trapezoidal}) {
            p.profile.method = method;
            for (auto &c : p.definitions[0].components)
                c.semiconductor.model =
                    ideal ? SemiconductorModel::ideal : SemiconductorModel::piecewise_linear;
            for (double delay : {.0005, .0025, .002503, .0075}) {
                p.patterns[0].delay = delay;
                p.patterns[1].delay = delay + .01;
                verify(p, ideal, delay);
                Document document(p);
                document.expand_instance(p.instances[0].id);
                verify(document.root_project(), ideal, delay);
                document.undo();
                check(document.root_project() == p, "Expansion undo");
            }
            auto blocked = p;
            for (auto &g : blocked.patterns)
                g.duty = 0;
            verify(blocked, ideal, .0075, false);
            std::ostringstream out;
            write_project(p, out);
            std::istringstream in(out.str());
            check(read_project(in) == p, "AC controller roundtrip");
            auto permuted = p;
            std::reverse(permuted.definitions[0].components.begin(),
                         permuted.definitions[0].components.end());
            std::reverse(permuted.wires.begin(), permuted.wires.end());
            const auto a = execute(compile(p)), b = execute(compile(permuted));
            check(a.samples.size() == b.samples.size(), "Deterministic step count");
            for (size_t k = 0; k < a.samples.size(); ++k)
                check(a.samples[k].values == b.samples[k].values, "Deterministic AC commutation");
        }
    std::cout << "PASS AC controller waveform, RMS, power, holding and blocking\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
