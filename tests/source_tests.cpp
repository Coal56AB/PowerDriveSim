#include "core/editor/document.hpp"
#include "core/editor/properties.hpp"
#include "core/model/hierarchy.hpp"
#include "core/model/waveform.hpp"
#include "core/solver/reference/reference.hpp"
#include "formats/project/project.hpp"
#include "formats/samples/table.hpp"
#include "results/csv.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <numbers>
#include <sstream>
using namespace pds;
static std::string id(int n) {
    return derived_uuid("source-test/" + std::to_string(n));
}
static void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
static void near(double a, double b, double tolerance, const char *message) {
    if (!std::isfinite(a) || std::abs(a - b) > tolerance)
        throw std::runtime_error(std::string(message) + ": " + std::to_string(a) +
                                 " != " + std::to_string(b));
}
static void error(const char *code, const std::function<void()> &operation) {
    try {
        operation();
    } catch (const Diagnostic &e) {
        check(e.code == code, "Wrong diagnostic");
        return;
    }
    throw std::runtime_error(std::string("Missing diagnostic ") + code);
}
static Project fixture(bool current = false) {
    Project p;
    p.id = id(0);
    p.name = "Source test";
    p.profile = {.05, 1e-5};
    p.nodes = {{id(1), "Ground", true}, {id(2), "Input", false}};
    p.components = {{id(10), "Source", current ? Kind::current : Kind::voltage, current ? id(1) : id(2),
                     current ? id(2) : id(1), 2},
                    {id(11), "Load", Kind::resistor, id(2), id(1), 10}};
    return p;
}
static size_t channel(const Result &r, int object) {
    auto i = std::find_if(r.channels.begin(), r.channels.end(),
                          [&](const auto &c) { return c.object == id(object); });
    check(i != r.channels.end(), "Channel missing");
    return size_t(i - r.channels.begin());
}
static void roundtrip(const Project &p) {
    std::ostringstream out;
    write_project(p, out);
    std::istringstream in(out.str());
    check(read_project(in) == p, "Waveform round trip");
}
int main(int argc, char **argv) try {
    check(argc == 2, "Pass repository root");
    auto parse_table = [](const std::string &text, const std::string &unit = "V") {
        std::istringstream in(text);
        return read_sample_table(in, unit);
    };
    for (const auto &text : {"time[s],value[V]\n0,1\n0.001,2\n",
                             "\xef\xbb\xbf# comment\r\ntime[ms];value[mV]\r\n0;1000\r\n1,0;2000\r\n",
                             "t\tvalue\n0\t1,0\n1ms\t2V\n", "# SI\n0s 1V\n1ms 2V\n",
                             "\"time[ms]\",\"Voltage, V[mV]\"\n\"0\",\"1 V\"\n\"1ms\",\"2000\"\n"})
        check(parse_table(text) == std::vector<Point>{{0, 1}, {.001, 2}}, "Sample table delimiters and units");
    check(parse_table("time[us],value[mA]\n0,1\n1000,2\n", "A") == std::vector<Point>{{0, .001}, {.001, .002}},
          "Current table header units");
    for (const auto &text :
         {"", "# empty\n", "time,value\n", "0,1,2\n", "-1,0\n", "0,1\n0,2\n", "1,1\n0,2\n", "0,nan\n",
          "0,inf\n", "1e999,1\n", "0,1A\n", "time[V],value[V]\n0,1\n", "time[s],value[A]\n0,1\n", "0,\"1\n",
          "0,\"1\"x\n", "time[s],value[V]\n0,1\ntime[s],value[V]\n"})
        error("invalid_samples", [&] { parse_table(text); });
    try {
        parse_table("# comment\n0,1\n0,2\n");
        check(false, "Missing table line diagnostic");
    } catch (const Diagnostic &e) {
        check(std::string(e.what()).find("Line 3:") != std::string::npos, "Table error locates row");
    }
    std::ostringstream long_table;
    for (size_t i = 0; i <= sample_table_max_rows; ++i)
        long_table << i << ",0\n";
    error("invalid_samples", [&] { parse_table(long_table.str()); });
    error("invalid_samples", [&] { parse_table(std::string(sample_table_max_bytes + 1, '#')); });
    Result exported;
    exported.channels.push_back({"voltage", "u:source, output; \"probe\"", "V"});
    exported.samples = {{0, {1.2345678901234567}, {}}, {.0012345678901234567, {2}, {}}};
    std::ostringstream csv;
    write_csv(exported, csv);
    check(parse_table(csv.str()) == std::vector<Point>{{0, 1.2345678901234567}, {.0012345678901234567, 2}},
          "Single-channel Scope CSV imports without loss");
    for (bool current : {false, true}) {
        auto p = fixture(current);
        auto &s = p.components[0].source;
        s.kind = Waveform::sine;
        s.offset = .3;
        s.phase = .4;
        s.frequency = 53;
        s.delay = .00017;
        auto result = execute(compile(p));
        const auto node = channel(result, 2), source = channel(result, 10);
        for (const auto &sample : result.samples) {
            const double expected =
                sample.time < s.delay
                    ? .3
                    : .3 + 2 * std::sin(2 * std::numbers::pi * 53 * (sample.time - s.delay) + .4);
            near(sample.values[node], expected * (current ? 10 : 1), 1e-12, "Sine source and Ohm law");
            if (current)
                near(sample.values[source], expected, 1e-12, "Dynamic current observation");
        }
        roundtrip(p);
    }
    for (auto method : {Method::backward_euler, Method::trapezoidal}) {
        auto p = fixture();
        p.profile.method = method;
        p.nodes.push_back({id(3), "Output", false});
        p.components[0].source.kind = Waveform::sine;
        p.components[1].negative = id(3);
        p.components.push_back({id(12), "C", Kind::capacitor, id(3), id(1), .001});
        auto result = execute(compile(p));
        const auto out = channel(result, 3);
        double maximum = 0;
        for (const auto &sample : result.samples) {
            const double omega = 2 * std::numbers::pi * 50, tau = .01;
            const double expected =
                2 / (1 + omega * omega * tau * tau) *
                (std::sin(omega * sample.time) - omega * tau * std::cos(omega * sample.time) +
                 omega * tau * std::exp(-sample.time / tau));
            maximum = std::max(maximum, std::abs(sample.values[out] - expected));
        }
        check(maximum < (method == Method::trapezoidal ? 1e-6 : .002), "RC sine analytical transient");
        std::cout << method_name(method) << " sine RC max error=" << maximum << '\n';

        // Current pulse into C: exact integral for either method on each constant interval.
        p = fixture(true);
        p.profile = {.0037, .00037, method};
        p.components[1].kind = Kind::capacitor;
        p.components[1].value = .001;
        p.components[0].source.kind = Waveform::pulse;
        p.components[0].source.frequency = 1000;
        p.components[0].source.duty = .27;
        p.components[0].source.delay = .00011;
        result = execute(compile(p));
        const auto voltage = channel(result, 2), amperes = channel(result, 10);
        double expected = 0;
        bool rise = false, fall = false;
        for (size_t i = 0; i < result.samples.size(); ++i) {
            const auto &sample = result.samples[i];
            if (i) {
                double previous = result.samples[i - 1].time;
                expected += source_value(p.components[0], (previous + sample.time) / 2) *
                            (sample.time - previous) / .001;
            }
            near(sample.values[voltage], expected, 2e-11, "Pulse integral has no pre-edge leakage");
            near(sample.values[amperes], source_value(p.components[0], sample.time), 1e-12,
                 "Right-continuous edge sample");
            rise |= sample.time == .00011;
            fall |= sample.time == .00011 + .27 / 1000;
        }
        check(rise && fall, "Off-grid pulse edges recorded exactly");
        roundtrip(p);
        p.components[0].source.duty = 0;
        result = execute(compile(p));
        near(result.samples.back().values[channel(result, 2)], 0, 0, "Zero duty");
        p.components[0].source.duty = 1;
        result = execute(compile(p));
        near(result.samples.back().values[channel(result, 2)], 2 * (p.profile.stop - .00011) / .001, 1e-11,
             "Full duty is one delayed step");
    }
    auto p = fixture(true);
    p.profile = {.003, .00037, Method::trapezoidal};
    p.components[1].kind = Kind::capacitor;
    p.components[1].value = .001;
    auto &table = p.components[0].source;
    table.kind = Waveform::piecewise_linear;
    table.points = {{0, 1}, {.00011, 2}, {.0008, -1}, {.0017, 0}};
    auto result = execute(compile(p));
    double integral = 0;
    for (size_t i = 1; i < result.samples.size(); ++i) {
        auto a = result.samples[i - 1].time, b = result.samples[i].time;
        integral +=
            (source_value(p.components[0], a) + source_value(p.components[0], b)) * .5 * (b - a) / .001;
        near(result.samples[i].values[channel(result, 2)], integral, 1e-11, "PWL exact trapezoidal integral");
    }
    for (const auto &point : table.points)
        check(std::any_of(result.samples.begin(), result.samples.end(),
                          [&](const auto &sample) { return sample.time == point.x; }),
              "Table corners recorded");
    roundtrip(p);
    table.points[1].x = 0;
    error("invalid_waveform", [&] { compile(p); });
    table.points.clear();
    error("invalid_waveform", [&] { compile(p); });
    p = fixture();
    p.components[0].source.frequency = 0;
    error("invalid_waveform", [&] { compile(p); });
    p = fixture();
    p.components[1].source.kind = Waveform::sine;
    error("invalid_waveform", [&] { compile(p); });

    // A simultaneous source edge and gate assignment is one algebraic update.
    p = fixture();
    p.profile = {.003, .00037};
    p.nodes.push_back({id(3), "Switched", false});
    p.components[1].positive = id(3);
    p.components.push_back({id(12), "Switch", Kind::ideal_switch, id(2), id(3), 0});
    p.components[0].source.kind = Waveform::pulse;
    p.components[0].source.frequency = 1000;
    p.components[0].source.delay = .00011;
    p.events = {{.00011, id(12), true}, {.00011 + .5 / 1000, id(12), false}};
    result = execute(compile(p));
    for (const auto &sample : result.samples)
        near(sample.values[channel(result, 3)],
             sample.time >= p.events[0].time && sample.time < p.events[1].time ? 2 : 0, 1e-12,
             "Simultaneous gate and source");

    p = fixture();
    p.components[0].source.kind = Waveform::sine;
    Document document(p);
    auto instance = document.create_definition({id(10), id(11)}, "Driven load");
    auto definition_id = document.root_project().instances.front().definition;
    document.edit_definition(definition_id, [&](Definition &d) {
        d.parameters.push_back({id(50), "Frequency", "Hz", id(10), "source_frequency", 50});
    });
    document.apply("Set instance frequency",
                   [&](Project &project) { write_property(project, instance, "parameter/" + id(50), 100.); });
    const auto flat = flatten(document.root_project()).project;
    check(std::any_of(flat.components.begin(), flat.components.end(),
                      [](const auto &c) { return c.kind == Kind::voltage && c.source.frequency == 100; }),
          "Public source parameter reaches solver");
    roundtrip(document.root_project());
    for (const auto *mode : {"sine", "pulse", "table"}) {
        std::ifstream input(std::string(argv[1]) + "/examples/rc-" + mode + ".pds");
        auto project = read_project(input);
        auto run = execute(compile(project));
        const auto source = std::find_if(project.components.begin(), project.components.end(),
                                         [](const auto &c) { return c.kind == Kind::voltage; });
        const auto output = std::find_if(run.channels.begin(), run.channels.end(),
                                         [](const auto &c) { return c.name == "u:output"; });
        check(source != project.components.end() && output != run.channels.end(),
              "Example source and output");
        const size_t index = size_t(output - run.channels.begin());
        double expected = 0, maximum = 0;
        for (size_t i = 0; i < run.samples.size(); ++i) {
            const auto t = run.samples[i].time;
            const double tau = .01, omega = 2 * std::numbers::pi * 50;
            if (std::string(mode) == "sine")
                expected = 2 / (1 + omega * omega * tau * tau) *
                           (std::sin(omega * t) - omega * tau * std::cos(omega * t) +
                            omega * tau * std::exp(-t / tau));
            else if (i) {
                const auto begin = run.samples[i - 1].time, h = t - begin;
                const double u = source_value(*source, begin), end = source_value(*source, t, TimeSide::left);
                const double slope = (end - u) / h;
                expected = u + slope * (h - tau) + (expected - u + slope * tau) * std::exp(-h / tau);
            }
            maximum = std::max(maximum, std::abs(run.samples[i].values[index] - expected));
        }
        check(maximum < 2e-6, "Bundled waveform example agrees with analytical RC response");
        std::cout << mode << " example RC max error=" << maximum << '\n';
    }
    std::cout << "PASS source waveforms, exact edges, analytical transients, serialization and hierarchy\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
