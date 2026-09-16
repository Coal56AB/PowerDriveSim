#include "core/editor/document.hpp"
#include "core/editor/properties.hpp"
#include "core/model/hierarchy.hpp"
#include "core/solver/reference/reference.hpp"
#include "formats/project/project.hpp"
#include "results/measurements.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
using namespace pds;
static std::string id(int n) {
    return derived_uuid("transistor-test/" + std::to_string(n));
}
static void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
static void near(double a, double b, double tol, const char *message) {
    if (!std::isfinite(a) || std::abs(a - b) > tol)
        throw std::runtime_error(std::string(message) + ": " + std::to_string(a) + " vs " +
                                 std::to_string(b));
}
static Project fixture() {
    Project p;
    p.id = id(0);
    p.name = "IGBT";
    p.profile = {.01, .001};
    p.nodes = {{id(1), "Ground", true}, {id(2), "Supply", false}, {id(3), "Load", false}};
    p.components = {{id(10), "Source", Kind::voltage, id(2), id(1), 5},
                    {id(11), "IGBT", Kind::igbt, id(2), id(3), 0},
                    {id(12), "R", Kind::resistor, id(3), id(1), 10}};
    return p;
}
static double value(const Result &r, const Sample &s, const std::string &key) {
    auto i =
        std::find_if(r.channels.begin(), r.channels.end(), [&](const auto &c) { return c.object == key; });
    check(i != r.channels.end(), "Missing result channel");
    return s.values[size_t(i - r.channels.begin())];
}
static std::string encode(const Project &p) {
    std::ostringstream out;
    write_project(p, out);
    return out.str();
}
int main(int argc, char **argv) try {
    check(argc == 2, "Pass repository root");
    for (bool pwl : {false, true})
        for (auto method : {Method::backward_euler, Method::trapezoidal})
            for (double voltage : {-5., -.1, 0., .69, .7, .707, .72, 5.})
                for (bool initial : {false, true}) {
                    auto p = fixture();
                    p.profile.method = method;
                    p.components[0].value = voltage;
                    p.components[1].closed = initial;
                    if (pwl)
                        p.components[1].semiconductor = {SemiconductorModel::piecewise_linear, .2, 1000, .7};
                    p.events = {
                        {.0023, id(11), !initial}, {.0027, id(11), initial}, {.0071, id(11), !initial}};
                    auto r = execute(compile(p));
                    for (const auto &s : r.samples) {
                        const bool gate =
                            (s.time >= .0023 && s.time < .0027) || s.time >= .0071 ? !initial : initial;
                        const bool forward = voltage > (pwl ? .7 * (1 + 10. / 1000) : 0);
                        const bool on = gate && forward;
                        const double expected =
                            on ? (pwl ? (voltage - .7 * (1 - .2 / 1000)) / 10.2 : voltage / 10)
                               : (pwl ? voltage / 1010 : 0);
                        const double current = value(r, s, id(11)), out = value(r, s, id(3));
                        near(current, expected, 1e-11, "IGBT gate and forward characteristic");
                        check(s.gates == std::vector<bool>{gate}, "IGBT gate channel");
                        near(out, current * 10, 1e-11, "Load KCL");
                        const double dissipation = (voltage - out) * current;
                        check(dissipation >= -1e-12, "IGBT passivity");
                        near(voltage * value(r, s, id(10)) + out * out / 10 + dissipation, 0, 1e-11,
                             "Power balance");
                    }
                    std::istringstream in(encode(p));
                    check(read_project(in) == p, "IGBT roundtrip");
                    std::reverse(p.components.begin(), p.components.end());
                    auto reordered = execute(compile(p));
                    for (size_t i = 0; i < r.samples.size(); ++i)
                        check(r.samples[i].values == reordered.samples[i].values,
                              "IGBT deterministic ordering");
                    check(result_channel(r, int(r.channels.size())).name == "gate:IGBT", "IGBT gate name");
                }
    auto p = fixture();
    auto old = encode(p);
    old.replace(0, old.find('\n'), "PowerDriveSim 11");
    try {
        std::istringstream in(old);
        read_project(in);
        check(false, "Old schema accepted IGBT");
    } catch (const Diagnostic &e) {
        check(e.code == "schema_version", "IGBT schema diagnostic");
    }
    p.components[1].semiconductor.initial_latched = true;
    try {
        compile(p);
        check(false, "IGBT accepted latch");
    } catch (const Diagnostic &e) {
        check(e.code == "invalid_thyristor", "Latch diagnostic");
    }
    p = fixture();
    Document d(p);
    const auto gate_pattern = d.add_pattern(0, -100);
    d.connect({gate_pattern, "out"}, {id(11), "gate"});
    d.apply("Gate pulse", [&](Project &q) { q.events = {{.0023, gate_pattern, true}, {.0027, gate_pattern, false}}; });
    const auto channel_instance = d.create_definition({id(11)}, "IGBT channel");
    auto r = execute(compile(d.root_project()));
    const auto channel_uuid = expanded_uuid({channel_instance}, id(11));
    for (const auto &s : r.samples)
        near(value(r, s, channel_uuid), s.time >= .0023 && s.time < .0027 ? .5 : 0, 1e-12,
             "IGBT gate through public port");
    const auto before = d.root_project();
    d.expand_instance(channel_instance);
    d.undo();
    check(d.root_project() == before, "IGBT expansion undo");
    for (bool mosfet : {false, true}) {
        std::ifstream input(std::string(argv[1]) + "/library/semiconductors/" +
                            (mosfet ? "mosfet.pds" : "igbt.pds"));
        const auto fragment = read_project(input);
        check(fragment.instances.size() == 1 && fragment.definitions.size() == 1,
              "Editable library definition");
        const auto &definition = fragment.definitions.front();
        check(definition.components.size() == 2, "Separate channel and diode");
        const auto drain = definition.ports[0].id, source = definition.ports[1].id,
                   gate = definition.ports[2].id;
        for (bool ideal : {false, true})
            for (bool on : {false, true})
                for (double supply : {-5., -.1, .1, 5.})
                    for (double load : {.01, 10.}) {
                        auto circuit = fixture();
                        circuit.components.erase(circuit.components.begin() + 1);
                        circuit.components[0].value = supply;
                        circuit.components[1].value = load;
                        Document document(circuit);
                        const auto instance = document.paste(fragment, 0, 0).front();
                        document.connect({instance, drain}, {id(2), "node"});
                        document.connect({instance, source}, {id(3), "node"});
                        const auto pattern = document.add_pattern(0, -100);
                        document.connect({pattern, "out"}, {instance, gate});
                        document.apply("Initial gate", [&](Project &q) { q.patterns[0].initial = on; });
                        document.edit_definition(definition.id, [&](Definition &d) {
                            for (auto &c : d.components)
                                c.semiconductor.model =
                                    ideal ? SemiconductorModel::ideal : SemiconductorModel::piecewise_linear;
                        });
                        double expected;
                        if (ideal)
                            expected = on || supply < 0 ? supply / load : 0;
                        else {
                            const bool forward = !mosfet && on && supply > .7 * (1 + load * 2e-6);
                            const double gc = (mosfet && on) || forward ? 100 : 1e-6;
                            const bool body = supply < -.7 * (1 + load * (gc + 1e-6));
                            const double gd = body ? 100 : 1e-6;
                            const double intercept = (body ? 1. : 0.) * .7 * (100 - 1e-6) -
                                                     (forward ? 1. : 0.) * .7 * (100 - 1e-6);
                            const double voltage = (supply - load * intercept) / (1 + load * (gc + gd));
                            expected = (supply - voltage) / load;
                        }
                        const auto result = execute(compile(document.root_project()));
                        for (const auto &s : result.samples)
                            near(-value(result, s, id(10)), expected, 1e-9,
                                 "Transistor and body-diode analytical current");
                        const auto saved = encode(document.root_project());
                        std::istringstream reload(saved);
                        check(read_project(reload) == document.root_project(), "Embedded library roundtrip");
                        document.expand_instance(instance);
                        const auto expanded = execute(compile(document.root_project()));
                        for (const auto &s : expanded.samples)
                            near(-value(expanded, s, id(10)), expected, 1e-9, "Editable expanded transistor");
                    }
        // Reinserting a stock template must preserve a user's edited definition.
        Project empty;
        empty.id = id(90);
        empty.wired = true;
        Document imports(empty);
        imports.paste(fragment, 0, 0);
        imports.edit_definition(definition.id,
                                [](Definition &d) { d.components.back().name = "Edited diode"; });
        imports.paste(fragment, 300, 0);
        check(imports.root_project().definitions.size() == 2, "Import conflict detaches stock definition");
        check(imports.root_project().instances[0].name != imports.root_project().instances[1].name,
              "Repeated library insertion has distinct instance names");
        check(imports.root_project().definitions.front().components.back().name == "Edited diode",
              "Import preserves edited diode");
        imports.undo();
        check(imports.root_project().instances.size() == 1 && imports.root_project().definitions.size() == 1,
              "Library placement atomic undo");
    }
    std::cout << "PASS ideal/PWL IGBT and editable MOSFET/IGBT modules, gate, reverse paths, energy, "
                 "persistence and hierarchy\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
