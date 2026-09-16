#include "core/editor/document.hpp"
#include "core/editor/properties.hpp"
#include "core/model/hierarchy.hpp"
#include "core/model/semiconductor.hpp"
#include "core/solver/reference/reference.hpp"
#include "formats/project/project.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
using namespace pds;
static std::string id(int n) {
    return derived_uuid("diode-charge/" + std::to_string(n));
}
static void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
static void near(double x, double y, double tolerance, const char *message) {
    if (!std::isfinite(x) || std::abs(x - y) > tolerance)
        throw std::runtime_error(std::string(message) + ": " + std::to_string(x) + " vs " +
                                 std::to_string(y));
}
static double value(const Result &r, const Sample &s, const std::string &object) {
    auto c =
        std::find_if(r.channels.begin(), r.channels.end(), [&](const auto &c) { return c.object == object; });
    check(c != r.channels.end(), "Channel missing");
    return s.values[size_t(c - r.channels.begin())];
}
static Project fixture() {
    Project p;
    p.id = id(0);
    p.name = "Charge control";
    p.profile = {.00018, 2.5e-8};
    p.nodes = {{id(1), "Ground", true}, {id(2), "Input", false}};
    p.components = {{id(10), "Source", Kind::voltage, id(2), id(1), 3},
                    {id(11), "Diode", Kind::diode, id(2), id(1), 0}};
    p.components[0].source = {Waveform::pulse, -2, 5000, 0, 0, .5, {}};
    p.components[1].semiconductor = {SemiconductorModel::piecewise_linear, 2, 1e5, .7, true, 1e-6, 5e-6, 0};
    return p;
}
static std::string encode(const Project &p) {
    std::ostringstream out;
    write_project(p, out);
    return out.str();
}
int main(int argc, char **argv) try {
    check(argc == 2, "Pass repository root");
    const double alpha = (1 + 5.) * (.5 - 1e-5), lambda = 1 / 1e-6 + 1 / 5e-6;
    const double qsteady = 5e-6 * (.5 - 1e-5) * .3, edge = .5 / 5000;
    for (auto method : {Method::backward_euler, Method::trapezoidal}) {
        double coarse = 0;
        for (double step : {2.5e-8, 1.25e-8}) {
            auto p = fixture();
            p.profile.method = method;
            p.profile.step = step;
            const auto r = execute(compile(p));
            double maximum = 0, balance = 0, previous_q = 0;
            bool saw_edge = false;
            auto power_minus_loss = [&](double v, double q) {
                const double extra = alpha * std::max(0., v - .7) - q / 1e-6;
                const double current = v / 1e5 + extra, u = .7 + q / (alpha * 1e-6);
                const double loss = v * v / 1e5 + (v > .7 ? 1 / alpha * extra * extra + u * q / 5e-6
                                                          : (u - v) * q / 1e-6 + u * q / 5e-6);
                check(loss >= -1e-15, "Charge model dissipation must be nonnegative");
                return v * current - loss;
            };
            for (size_t n = 0; n < r.samples.size(); ++n) {
                const auto &s = r.samples[n];
                const double v = value(r, s, id(2)), i = value(r, s, id(11));
                const double q = (v / 1e5 + alpha * std::max(0., v - .7) - i) * 1e-6;
                check(q >= -1e-20, "Stored charge is nonnegative");
                const double expected = s.time <= edge ? qsteady * (1 - std::exp(-lambda * s.time))
                                                       : qsteady * (1 - std::exp(-lambda * edge)) *
                                                             std::exp(-lambda * (s.time - edge));
                maximum = std::max(maximum, std::abs(q - expected));
                if (!n)
                    near(i, 1e-5 + alpha * .3, 1e-12, "Explicit uncharged start");
                if (s.time == edge) {
                    saw_edge = true;
                    near(q, qsteady, 1e-18, "Source edge preserves charge");
                    check(i < -.7, "Reverse current is present");
                }
                if (n) {
                    const double vleft = s.time <= edge ? 1 : -2;
                    balance += (s.time - r.samples[n - 1].time) *
                               (power_minus_loss(vleft, previous_q) + power_minus_loss(vleft, q)) / 2;
                }
                previous_q = q;
            }
            check(saw_edge, "Exact source edge recorded");
            const double energy = .7 * previous_q + previous_q * previous_q / (2 * alpha * 1e-6);
            near(balance, energy, method == Method::trapezoidal ? 2e-10 : 2e-8,
                 "Integrated charge energy balance");
            if (!coarse)
                coarse = maximum;
            else
                check(coarse / maximum > (method == Method::trapezoidal ? 3.8 : 1.9),
                      "Charge integration convergence order");
            check(maximum / qsteady < (method == Method::trapezoidal ? 3e-5 : .006),
                  "Analytical charge accumulation and decay");
            std::cout << method_name(method) << " h=" << step << " max charge error=" << maximum
                      << " energy residual=" << balance - energy << '\n';
        }
    }
    auto p = fixture();
    p.components[0].source = {};
    p.components[0].value = -2;
    p.components[1].semiconductor.initial_charge = 1e-7;
    p.profile = {1e-5, 3e-6, Method::trapezoidal};
    try {
        execute(compile(p));
        check(false, "Coarse trapezoidal must diagnose negative charge");
    } catch (const Diagnostic &e) {
        check(e.code == "invalid_charge_state" && e.object == id(11), "Addressed charge diagnostic");
    }
    p.profile.method = Method::backward_euler;
    auto r = execute(compile(p));
    near(value(r, r.samples.front(), id(11)), -2e-5 - .1, 1e-12, "Initial charge creates reverse current");
    for (const auto &s : r.samples)
        check(value(r, s, id(11)) <= -2e-5, "BE coarse-step decay stays physical");
    auto serialized = encode(p);
    std::istringstream input(serialized);
    check(read_project(input) == p, "Charge round trip");
    auto old = serialized;
    old.replace(0, old.find('\n'), "PowerDriveSim 9");
    try {
        std::istringstream in(old);
        read_project(in);
        check(false, "v9 must reject charge semantics");
    } catch (const Diagnostic &e) {
        check(e.code == "parse_error", "Charge requires schema10");
    }
    const auto duplicate = serialized + "diode_charge \"" + id(11) + "\" 1 1e-6 5e-6 0\n";
    try {
        std::istringstream in(duplicate);
        read_project(in);
        check(false, "Duplicate charge model accepted");
    } catch (const Diagnostic &e) {
        check(e.code == "parse_error", "Duplicate charge diagnostic");
    }
    auto no_charge = fixture();
    no_charge.components[1].semiconductor.charge_dynamics = false;
    const auto wrong_target = encode(no_charge) + "diode_charge \"" + id(10) + "\" 1 1e-6 5e-6 0\n";
    try {
        std::istringstream in(wrong_target);
        read_project(in);
        check(false, "Source accepted diode charge");
    } catch (const Diagnostic &e) {
        check(e.code == "invalid_diode_charge", "Wrong charge target diagnostic");
    }
    Document document(p);
    auto original = document.root_project();
    document.apply("Charge parameter", [&](Project &q) { write_property(q, id(11), "transit_time", 2e-6); });
    document.undo();
    check(document.root_project() == original, "Charge property undo");
    document.redo();
    auto instance = document.create_definition({id(11)}, "Diode");
    auto definition_id = document.root_project().instances.front().definition;
    document.edit_definition(definition_id, [&](Definition &d) {
        d.parameters.push_back({id(50), "Lifetime", "s", id(11), "carrier_lifetime", 5e-6});
    });
    document.apply("Instance lifetime",
                   [&](Project &q) { write_property(q, instance, "parameter/" + id(50), 8e-6); });
    const auto expanded = flatten(document.root_project()).project;
    check(std::any_of(expanded.components.begin(), expanded.components.end(),
                      [](const auto &c) {
                          return c.kind == Kind::diode && c.semiconductor.carrier_lifetime == 8e-6;
                      }),
          "Charge parameter flatten");
    execute(compile(document.root_project()));
    for (const auto *key : {"transit_time", "carrier_lifetime", "initial_charge"}) {
        auto invalid = fixture();
        *semiconductor_parameter(invalid.components[1].semiconductor, key) = -1;
        try {
            compile(invalid);
            check(false, "Invalid charge accepted");
        } catch (const Diagnostic &e) {
            check(e.code == "invalid_diode_charge", "Charge validation code");
        }
    }
    // Disabling dynamics keeps the static PWL result independent of stored charge settings.
    p.components[1].semiconductor.charge_dynamics = false;
    r = execute(compile(p));
    for (const auto &s : r.samples)
        near(value(r, s, id(11)), -2e-5, 1e-15, "Disabled charge uses static PWL");
    std::ifstream example(std::string(argv[1]) + "/examples/diode-freewheel.pds");
    p = read_project(example);
    for (auto &c : p.components)
        if (c.kind == Kind::diode) {
            c.semiconductor.model = SemiconductorModel::piecewise_linear;
            c.semiconductor.charge_dynamics = true;
            c.semiconductor.transit_time = 1e-4;
            c.semiconductor.carrier_lifetime = 5e-4;
        }
    r = execute(compile(p));
    check(r.last_time == p.profile.stop, "Generic freewheel charge run");
    std::ifstream recovery(std::string(argv[1]) + "/examples/diode-recovery.pds");
    p = read_project(recovery);
    r = execute(compile(p));
    const auto diode = std::find_if(p.components.begin(), p.components.end(),
                                    [](const auto &c) { return c.kind == Kind::diode; });
    const auto probe = std::find_if(p.components.begin(), p.components.end(),
                                    [](const auto &c) { return c.kind == Kind::current_probe; });
    check(diode != p.components.end() && probe != p.components.end(), "Recovery example atoms");
    for (const auto &s : r.samples) {
        const double q = s.time <= edge
                             ? qsteady * (1 - std::exp(-lambda * s.time))
                             : qsteady * (1 - std::exp(-lambda * edge)) * std::exp(-lambda * (s.time - edge));
        const double v = s.time < edge ? 1 : -2;
        const double expected = v / 1e5 + alpha * std::max(0., v - .7) - q / 1e-6;
        near(value(r, s, diode->id), expected, 2.1e-5, "Recovery example analytical current");
        near(value(r, s, probe->id), value(r, s, diode->id), 1e-12, "Recovery current probe");
    }
    std::cout
        << "PASS charge control, reverse recovery, convergence, energy, events, persistence and hierarchy\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
