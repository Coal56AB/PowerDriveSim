#include "core/editor/document.hpp"
#include "core/solver/reference/reference.hpp"
#include "formats/project/project.hpp"
#include "results/measurements.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
using namespace pds;
static void check(bool valid, const char *message) {
    if (!valid)
        throw std::runtime_error(message);
}
int main(int argc, char **argv) {
    try {
        if (argc < 2)
            return 2;
        std::ifstream file(std::string(argv[1]) + "/examples/rc.pds");
        Document doc(read_project(file));
        auto base = doc.project();
        auto ir = compile(base);
        auto full = execute(ir);
        Recording off;
        off.all = false;
        auto empty = execute(ir, nullptr, nullptr, &off);
        check(empty.samples.empty() && empty.samples.capacity() == 0 && empty.channels.empty() &&
                  empty.gate_objects.empty(),
              "Disabled recording allocates no sample history");
        check(empty.accepted_steps == full.accepted_steps && empty.last_time == full.last_time &&
                  empty.max_scaled_residual == full.max_scaled_residual,
              "Recording must not change physical execution");
        std::ostringstream v5;
        write_project(base, v5);
        std::istringstream lines(v5.str());
        std::string line;
        std::ostringstream v4;
        std::getline(lines, line);
        v4 << "PowerDriveSim 4\n";
        while (std::getline(lines, line))
            if (line.rfind("scope_enabled ", 0) != 0 && line.rfind("initialization ", 0) != 0)
                v4 << line << '\n';
        std::istringstream old(v4.str());
        auto migrated = read_project(old);
        check(migrated.schema == project_schema && !migrated.scope_enabled && migrated.plots.empty(),
              "Schema 4 migrates with recording disabled");
        auto probe = doc.add_component(Kind::voltage_probe, 400, 200);
        doc.connect({probe, "p"}, {base.nodes[2].id, "node"});
        doc.connect({probe, "n"}, {base.nodes[0].id, "node"});
        auto signal = doc.add_pattern(0, -100);
        auto a = doc.add_plot(600, 100), b = doc.add_plot(600, 300);
        doc.connect({probe, "out"}, {a, "in1"});
        doc.connect({signal, "out"}, {a, "in2"});
        doc.connect({probe, "out"}, {b, "in1"});
        doc.apply("Pattern and plot view", [&](Project &p) {
            p.patterns[0].name = "Firing pulse";
            p.events = {{.001, signal, true}, {.002, signal, false}};
            p.plots[0].begin = .001;
            p.plots[0].cursor_a = .0015;
        });
        check(plot_channels(doc.project(), a).size() == 2 && plot_channels(doc.project(), b).size() == 1,
              "Independent plot bindings");
        Recording selected;
        selected.all = false;
        selected.channels = {probe, "gate/" + signal};
        auto result = execute(compile(doc.project()), nullptr, nullptr, &selected);
        check(result.channels.size() == 1 && result.gate_objects.size() == 1,
              "Only requested analog and digital channels stored");
        check(result_channel(result, 1).name == "Firing pulse", "Gate display name survives recording");
        auto digital = select_result(result, {"gate/" + signal});
        check(result_channel(digital, 0).name == "Firing pulse", "Gate name survives subset selection");
        bool streamed = false;
        execute(compile(doc.project()), nullptr, nullptr, &selected, nullptr, [&](Result &&batch) {
            check(result_channel(batch, 1).name == "Firing pulse", "Gate name survives live batches");
            streamed = true;
        });
        check(streamed, "Named live batch emitted");
        check(std::abs(result.samples.back().values[0] - (1 - std::exp(-5.0))) < 2e-5,
              "Plot probe is non-loading");
        bool saw_edge = false;
        for (const auto &sample : result.samples)
            if (sample.time == .001) {
                check(sample.gates[0], "Unconnected pattern edge is recorded exactly");
                saw_edge = true;
            }
        check(saw_edge, "Scheduled recording-only edge exists");
        auto voltage_only = select_result(result, {probe});
        check(voltage_only.channels.size() == 1 && voltage_only.gate_objects.empty() &&
                  voltage_only.samples[0].gates.empty(),
              "Export subset contains no unrelated gate values");
        check(select_result(result, {}).samples.capacity() == 0, "Clearing recording releases all history");
        std::ostringstream saved;
        write_project(doc.project(), saved);
        std::istringstream in(saved.str());
        auto loaded = read_project(in);
        check(loaded.schema == project_schema && loaded.plots.size() == 2 && !loaded.scope_enabled,
              "Schema 5 roundtrip defaults recording off");
        check(loaded.plots[0].cursor_a == .0015 && loaded.plots[1].cursor_a == -1,
              "Independent plot cursors roundtrip");
        doc.connect({base.components[0].id, "p"}, {b, "in2"});
        check(plot_channels(doc.project(),b).size()==2,"Direct electrical plot tap accepted");
        auto tapped=execute(compile(doc.project()));
        check(tapped.accepted_steps==result.accepted_steps,"Plot taps do not alter solver steps");
        doc.erase({a});
        check(doc.project().plots.size() == 1, "Delete plot");
        doc.undo();
        check(doc.project().plots.size() == 2 && doc.project().plots[0].id == a,
              "Undo restores plot IDs and wires");
        auto pwm=doc.add_pattern(100,100);
        doc.apply("PWM",[&](Project& p){auto& g=p.patterns.back();g.pwm=true;g.frequency=1000;g.duty=.25;g.delay=.0001;});
        Recording pulses;pulses.all=false;pulses.channels={"gate/"+pwm};
        auto pulse_result=execute(compile(doc.project()),nullptr,nullptr,&pulses);
        bool rise=false,fall=false;
        for(const auto& sample:pulse_result.samples){if(sample.time==.0001){check(sample.gates[0],"PWM rising edge");rise=true;}if(sample.time==(.0001+.25/1000)){check(!sample.gates[0],"PWM falling edge");fall=true;}}
        check(rise&&fall,"PWM schedules exact edges independently of timestep");
        doc.transform({pwm},1,true);
        std::ostringstream rotated;write_project(doc.project(),rotated);std::istringstream reload(rotated.str());auto restored=read_project(reload);
        check(restored.patterns.back().pwm&&restored.patterns.back().frequency==1000&&restored.patterns.back().orientation.mirrored,"PWM and orientation roundtrip");
        auto fragment=doc.copy({pwm});auto pasted=doc.paste(fragment,40,40);
        check(pasted.size()==1&&pasted[0]!=pwm&&doc.project().patterns.back().pwm,"Copy preserves PWM and assigns independent UUID");
        doc.undo();check(doc.project().patterns.back().id==pwm,"Paste undo");
        std::cout << "PASS plots, schema5, selective recording, no-history execution, signal edges and "
                     "export subsets\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
