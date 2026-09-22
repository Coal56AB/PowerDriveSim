#include "core/editor/document.hpp"
#include "core/model/hierarchy.hpp"
#include "core/solver/reference/reference.hpp"
#include "formats/project/project.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
using namespace pds;
static void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
static std::array<std::array<int, 3>, 6> states(bool npc) {
    if (npc)
        return {{{1, 0, -1}, {0, 1, -1}, {-1, 1, 0}, {-1, 0, 1}, {0, -1, 1}, {1, -1, 0}}};
    return {{{1, -1, -1}, {1, 1, -1}, {-1, 1, -1}, {-1, 1, 1}, {-1, -1, 1}, {1, -1, 1}}};
}
static double analytical_current(double time, unsigned phase, bool npc) {
    double current = 0;
    const auto schedule = states(npc);
    const auto intervals = static_cast<size_t>(std::ceil(time / .001 - 1e-12));
    for (size_t interval = 0; interval < intervals; ++interval) {
        const auto duration = std::clamp(time - double(interval) * .001, 0., .001);
        const auto target = 15. * schedule[interval % schedule.size()][phase];
        current = target + (current - target) * std::exp(-duration / .001);
    }
    return current;
}
static size_t named_channel(const Result &result, const std::string &name) {
    for (size_t i = 0; i < result.channels.size(); ++i)
        if (result.channels[i].name == name)
            return i;
    throw std::runtime_error("Missing channel " + name);
}
static void verify_response(const Project &project, bool npc) {
    auto ir = compile(project);
    const auto result = execute(ir);
    require(result.accepted_steps >= 600 && !result.cancelled, "Converter run must finish");
    const auto schedule = states(npc);
    double current_error = 0, voltage_error = 0;
    for (unsigned phase = 0; phase < 3; ++phase) {
        const auto suffix = std::string(1, char('A' + phase));
        const auto u = named_channel(result, "u:U" + suffix), i = named_channel(result, "i:I" + suffix);
        for (const auto &sample : result.samples) {
            const auto interval = size_t((sample.time + 1e-12) / .001) % schedule.size();
            voltage_error =
                std::max(voltage_error, std::abs(sample.values[u] - 300. * schedule[interval][phase]));
            current_error = std::max(
                current_error, std::abs(sample.values[i] - analytical_current(sample.time, phase, npc)));
        }
    }
    require(voltage_error < 1e-6, "Pole voltages follow the periodic programmable switch states");
    require(current_error < (project.profile.method == Method::backward_euler ? .08 : .0002),
            "Phase currents agree with the piecewise analytical RL transient");
    const auto resolved = resolve_connections(project).project;
    std::map<std::string, size_t> channel;
    for (size_t i = 0; i < result.channels.size(); ++i)
        channel[result.channels[i].object] = i;
    for (const auto &sample : result.samples) {
        double power = 0;
        for (const auto &component : resolved.components) {
            if (component.kind == Kind::voltage_probe)
                continue; // A voltage observation is not a branch-current channel.
            const double voltage = sample.values.at(channel.at(component.positive)) -
                                   sample.values.at(channel.at(component.negative));
            power += voltage * sample.values.at(channel.at(component.id));
        }
        require(std::abs(power) < 1e-5, "Converter instantaneous power balance");
    }
    for (const auto &component : resolved.components)
        if (component.kind == Kind::diode)
            for (const auto &sample : result.samples) {
                const auto voltage = sample.values[channel.at(component.positive)] -
                                     sample.values[channel.at(component.negative)];
                const auto current = sample.values[channel.at(component.id)];
                require(voltage < 1e-6 && current > -1e-7 && std::abs(voltage * current) < 1e-5,
                        "All antiparallel and clamping diodes satisfy complementarity");
            }
    auto permuted = project;
    std::reverse(permuted.components.begin(), permuted.components.end());
    std::reverse(permuted.events.begin(), permuted.events.end());
    std::reverse(permuted.wires.begin(), permuted.wires.end());
    std::reverse(permuted.definitions.begin(), permuted.definitions.end());
    for (auto &definition : permuted.definitions) {
        std::reverse(definition.components.begin(), definition.components.end());
        std::reverse(definition.wires.begin(), definition.wires.end());
    }
    const auto repeated = execute(compile(permuted));
    require(result.samples.size() == repeated.samples.size(), "File order preserves converter step count");
    for (size_t i = 0; i < result.samples.size(); ++i)
        require(result.samples[i].values == repeated.samples[i].values &&
                    result.samples[i].gates == repeated.samples[i].gates,
                "Converter output is bit-identical under object/event permutation");
    std::cout << (npc ? "NPC" : "2L") << ' ' << method_name(project.profile.method)
              << ": current error=" << current_error << " A, voltage error=" << voltage_error
              << " V, solves=" << result.linear_solves << '\n';
}
static void verify_editing(Project project, bool npc) {
    const auto flat = flatten(project).project;
    const auto count = [&](Kind kind) {
        return std::count_if(flat.components.begin(), flat.components.end(),
                             [&](const auto &c) { return c.kind == kind; });
    };
    require(count(Kind::ideal_switch) == (npc ? 12 : 6), "Every converter switch is an atomic component");
    require(count(Kind::diode) == (npc ? 18 : 6), "Antiparallel and clamping diodes are separate atoms");
    require(count(Kind::capacitor) == (npc ? 14 : 8),
            "Each switch has an individual capacitor plus two DC-link capacitors");
    require(flat.patterns.size() == size_t(npc ? 12 : 6), "Every switch has an external gate source");
    require(project.definitions.size() == 5,
            "Converter opens into three independently editable phase definitions");
    const auto converter_id = project.instances.front().id;
    const auto converter = definition(project, project.instances.front().definition);
    require(converter.instances.size() == 3, "Three phase legs");
    Document document(project);
    document.navigate({converter_id, converter.instances[0].id});
    const auto snubber =
        std::find_if(document.project().components.begin(), document.project().components.end(),
                     [](const auto &c) { return c.name == "Rsn1"; })
            ->id;
    document.apply("Change one snubber", [&](Project &body) {
        for (auto &c : body.components)
            if (c.id == snubber)
                c.value = 150;
    });
    auto edited = flatten(document.root_project()).project;
    require(std::count_if(edited.components.begin(), edited.components.end(),
                          [](const auto &c) {
                              return c.name.find("Rsn1") != std::string::npos && c.value == 150;
                          }) == 1,
            "Editing phase A does not change phase B or C");
    const auto parallel = document.add_component(Kind::resistor, 400, -200);
    document.apply("Set physical bleed resistor", [&](Project &body) {
        for (auto &c : body.components)
            if (c.id == parallel)
                c.value = 100000;
    });
    document.connect({parallel, "p"}, {snubber, "p"});
    document.connect({parallel, "n"}, {snubber, "n"});
    verify_response(document.root_project(), npc);
    std::ostringstream saved;
    write_project(document.root_project(), saved);
    std::istringstream restored(saved.str());
    require(read_project(restored) == document.root_project(),
            "Edited converter round trips from its internal view");
    document.navigate({});
    document.expand_instance(converter_id);
    const auto after = execute(compile(document.root_project()));
    require(!after.cancelled && after.last_time == project.profile.stop,
            "Expanded edited converter uses the same generic execution path");
    document.undo();
    require(document.root_project().instances.size() == 2, "Undo restores the converter hierarchy");
}
int main(int argc, char **argv) try {
    require(argc == 2, "Pass the repository root");
    for (bool npc : {false, true}) {
        std::ifstream input(std::string(argv[1]) + "/examples/" + (npc ? "npc-3l.pds" : "vsi-2l.pds"));
        auto project = read_project(input);
        std::ifstream library_input(std::string(argv[1]) + "/library/converters/" +
                                    (npc ? "npc-3l.pds" : "vsi-2l.pds"));
        const auto library = read_project(library_input);
        require(library.instances.size() == 1 && library.components.empty() && library.patterns.empty() &&
                    library.definitions.size() == 4,
                "Library fragment has the converter and three phase definitions without external sources");
        for (const auto &body : library.definitions)
            require(definition(project, body.id) == body, "Example and library share exactly the same definitions");
        require(definition(library, library.instances.front().definition).parameters.size() == 6,
                "DC-link capacitors, initial states and ESR are public parameters");
        Project inserted; inserted.id = new_uuid(); inserted.wired = true;
        Document palette(inserted);
        const auto added = palette.paste(library, 40, 80);
        require(added.size() == 1 && palette.root_project().instances.size() == 1, "Insert full converter fragment");
        palette.expand_instance(added.front());
        require(palette.root_project().instances.empty(), "Library converter expands to atoms");
        palette.undo(); require(palette.root_project().instances.size() == 1, "Undo library expansion");
        auto start = std::chrono::steady_clock::now();
        auto ir = compile(project);
        const auto compiled = std::chrono::steady_clock::now();
        std::atomic_bool stop{true};
        Recording recording{false, {}};
        auto initialized = execute(ir, &stop, nullptr, &recording);
        const auto end = std::chrono::steady_clock::now();
        require(initialized.cancelled && initialized.accepted_steps == 0,
                "Initialization can stop before the first step");
        std::cout << (npc ? "NPC" : "2L")
                  << ": compile=" << std::chrono::duration<double, std::milli>(compiled - start).count()
                  << " ms, initialize=" << std::chrono::duration<double, std::milli>(end - compiled).count()
                  << " ms\n";
        for (auto method : {Method::backward_euler, Method::trapezoidal}) {
            project.profile.method = method;
            verify_response(project, npc);
        }
        auto extended = project;
        extended.profile.method = Method::backward_euler;
        extended.profile.stop = .06;
        verify_response(extended, npc);
        verify_editing(project, npc);
        auto shorted = project;
        for (auto &body : shorted.definitions)
            for (auto &gate : body.patterns)
                gate.code = "return 1;";
        bool diagnosed = false;
        try {
            (void)execute(compile(shorted));
        } catch (const Diagnostic &e) {
            diagnosed = (e.code == "conflicting_voltage_constraints" || e.code == "nonlinear_convergence") &&
                         !e.object.empty();
            if (!diagnosed) throw;
        }
        require(diagnosed, "All gates on diagnoses a DC-bus short circuit");
    }
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
