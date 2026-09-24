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
    std::ifstream three_phase_input(std::string(argv[1]) +
                                    "/library/converters/ac-voltage-controller-3p.pds");
    auto three_phase = read_project(three_phase_input);
    validate_hierarchy(three_phase);
    check(three_phase.instances.size() == 1 && three_phase.definitions.size() == 2,
          "Three-phase controller has two editable hierarchy levels");
    check(three_phase.definitions[0].instances.size() == 3 &&
              three_phase.definitions[0].ports.size() == 12,
          "Three phases and independent power/gate terminals");
    const auto three_phase_flat = flatten(three_phase).project;
    check(three_phase_flat.instances.empty() &&
              std::count_if(three_phase_flat.components.begin(), three_phase_flat.components.end(),
                            [](const auto &component) {
                                return component.kind == Kind::thyristor;
                            }) == 6,
          "Three-phase controller flattens to six atomic thyristors");
    {
        Document document(three_phase);
        document.expand_instance(three_phase.instances.front().id);
        check(document.root_project().instances.empty() &&
                  std::count_if(document.root_project().components.begin(),
                                document.root_project().components.end(), [](const auto &component) {
                                    return component.kind == Kind::thyristor;
                                }) == 6,
              "Three-phase controller expands to editable atomic thyristors");
        check(flatten(document.root_project()).project == three_phase_flat,
              "Interactive expansion preserves the flattened circuit");
        document.undo();
        check(document.root_project() == three_phase, "Three-phase expansion undo");
    }
    std::ostringstream three_phase_output;
    write_project(three_phase, three_phase_output);
    std::istringstream three_phase_saved(three_phase_output.str());
    check(read_project(three_phase_saved) == three_phase, "Three-phase controller roundtrip");

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
    std::ifstream code_input(std::string(argv[1]) + "/examples/ac-voltage-controller.pds");
    auto code_driven = read_project(code_input);
    const auto positive_gate = code_driven.patterns[0].id;
    const auto negative_gate = code_driven.patterns[1].id;
    CodeBlock firing;
    firing.id = derived_uuid("ac-controller-code-firing");
    firing.name = "Three-phase firing";
    firing.period = 10e-6;
    firing.code = R"(double delay = ramp(0, 10, 0.008333333333, 0.001111111111);
gAp = phasepwm(50, 0.02, delay);
gAm = phasepwm(50, 0.02, delay + 0.010000000000);
gBp = phasepwm(50, 0.02, delay + 0.006666666667);
gBm = phasepwm(50, 0.02, delay + 0.016666666667);
gCp = phasepwm(50, 0.02, delay + 0.013333333333);
gCm = phasepwm(50, 0.02, delay + 0.023333333333);)";
    for (const auto *name : {"gAp", "gAm", "gBp", "gBm", "gCp", "gCm"})
        firing.outputs.push_back({derived_uuid(std::string("ac-controller-code-") + name), name, "",
                                  SignalScalarType::boolean, 0});
    for (auto &wire : code_driven.wires) {
        if (wire.from.object == positive_gate)
            wire.from = {firing.id, firing.outputs[0].id};
        if (wire.from.object == negative_gate)
            wire.from = {firing.id, firing.outputs[1].id};
    }
    code_driven.patterns.clear();
    code_driven.code_blocks.push_back(firing);
    const auto driven = execute(compile(code_driven));
    const auto voltage = std::find_if(driven.channels.begin(), driven.channels.end(),
                                      [](const auto &channel) { return channel.name == "u:Uload"; });
    check(voltage != driven.channels.end(), "CodeBlock-driven AC controller records load voltage");
    const auto voltage_index = static_cast<size_t>(voltage - driven.channels.begin());
    auto output_at = [&](double time) {
        const auto sample = std::find_if(driven.samples.begin(), driven.samples.end(), [&](const auto &entry) {
            return std::abs(entry.time - time) < 1e-12;
        });
        check(sample != driven.samples.end(), "CodeBlock-driven AC controller records the requested time");
        return sample->values[voltage_index];
    };
    check(std::abs(output_at(.005)) < 1, "Thyristors block before the CodeBlock firing pulse");
    check(output_at(.0085) > 20, "Positive thyristor conducts after the CodeBlock firing pulse");
    check(output_at(.0185) < -20, "Negative thyristor conducts after the CodeBlock firing pulse");
    auto three_phase_driven = three_phase;
    three_phase_driven.id = derived_uuid("three-phase-code-example");
    three_phase_driven.name = "Three-phase AC controller with CodeBlock";
    three_phase_driven.profile.stop = .04;
    three_phase_driven.profile.step = 10e-6;
    const auto neutral = derived_uuid("three-phase-code-neutral");
    three_phase_driven.nodes.push_back({neutral, "Neutral", true});
    three_phase_driven.nodes.back().x = 0;
    three_phase_driven.nodes.back().y = 420;
    three_phase_driven.instances.front().x = 0;
    three_phase_driven.instances.front().y = 0;
    const auto phase_plot = derived_uuid("three-phase-code-phase-plot");
    three_phase_driven.plots.push_back({phase_plot, "Phase load voltages", 840, -430, 3});
    firing.x = -320;
    firing.y = -420;
    three_phase_driven.code_blocks.push_back(firing);
    const auto &module = definition(three_phase_driven, three_phase_driven.instances.front().definition);
    auto module_port = [&](const std::string &name) {
        const auto found = std::find_if(module.ports.begin(), module.ports.end(),
                                        [&](const auto &entry) { return entry.name == name; });
        check(found != module.ports.end(), "Three-phase controller public port exists");
        return found->id;
    };
    auto connect = [&](const std::string &name, Endpoint from, Endpoint to) {
        three_phase_driven.wires.push_back({derived_uuid("three-phase-code-" + name),
                                            std::move(from), std::move(to)});
    };
    const auto module_id = three_phase_driven.instances.front().id;
    std::vector<std::string> loads;
    for (size_t phase = 0; phase < 3; ++phase) {
        const std::string label(1, char('A' + phase));
        Component source;
        source.id = derived_uuid("three-phase-code-supply-" + label);
        source.name = "Supply " + label;
        source.kind = Kind::voltage;
        source.value = 100;
        source.source.kind = Waveform::sine;
        source.source.frequency = 50;
        source.source.phase = phase == 1 ? -2 * std::numbers::pi / 3
                                         : phase == 2 ? 2 * std::numbers::pi / 3 : 0;
        source.x = -520;
        source.y = int(phase) * 220 - 220;
        Component load;
        load.id = derived_uuid("three-phase-code-load-" + label);
        load.name = "Load " + label;
        load.kind = Kind::resistor;
        load.value = 10;
        load.x = 520;
        load.y = int(phase) * 220 - 220;
        three_phase_driven.components.push_back(source);
        three_phase_driven.components.push_back(load);
        loads.push_back(load.id);
        connect("supply-p-" + label, {source.id, "p"}, {module_id, module_port(label)});
        connect("supply-n-" + label, {source.id, "n"}, {neutral, "node"});
        connect("load-p-" + label, {module_id, module_port(label + "'")}, {load.id, "p"});
        connect("load-n-" + label, {load.id, "n"}, {neutral, "node"});
        connect("plot-" + label, {load.id, "p"}, {phase_plot, "in" + std::to_string(phase + 1)});
    }
    for (size_t gate = 0; gate < firing.outputs.size(); ++gate) {
        const std::string label(1, char('A' + gate / 2));
        connect("gate-" + std::to_string(gate), {firing.id, firing.outputs[gate].id},
                {module_id, module_port("g" + label + (gate % 2 ? "-" : "+"))});
    }
    std::ostringstream code_saved;
    write_project(three_phase_driven, code_saved);
    std::istringstream code_reopened(code_saved.str());
    auto code_restored = read_project(code_reopened);
    check(code_restored == three_phase_driven, "Three-phase CodeBlock and gate wiring survive save/reopen");
    std::ifstream shipped_input(std::string(argv[1]) + "/examples/ac-voltage-controller-3p-code.pds");
    check(bool(shipped_input), "Three-phase CodeBlock example is shipped");
    const auto shipped = read_project(shipped_input);
    check(shipped == code_restored, "Shipped three-phase CodeBlock example matches the verified circuit");
    const auto phases = execute(compile(shipped));
    auto current_at = [&](size_t phase, double time) {
        const auto channel = std::find_if(phases.channels.begin(), phases.channels.end(),
                                          [&](const auto &entry) { return entry.object == loads[phase]; });
        check(channel != phases.channels.end(), "Three-phase load current channel exists");
        const auto sample = std::find_if(phases.samples.begin(), phases.samples.end(), [&](const auto &entry) {
            return std::abs(entry.time - time) < 1e-12;
        });
        check(sample != phases.samples.end(), "Three-phase gate checkpoint exists");
        return sample->values[size_t(channel - phases.channels.begin())];
    };
    check(std::abs(current_at(0, .005)) < .1, "Phase A blocks before firing");
    check(current_at(0, .0085) > 2 && current_at(0, .0185) < -2,
          "CodeBlock drives positive and negative phase A thyristors");
    check(current_at(1, .0152) > 2 && current_at(1, .0252) < -2,
          "CodeBlock drives positive and negative phase B thyristors");
    check(current_at(2, .0219) > 2 && current_at(2, .0118) < -2,
          "CodeBlock drives positive and negative phase C thyristors");
    std::cout << "PASS AC controller hierarchy, waveform, RMS, power, holding and six CodeBlock gates\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
