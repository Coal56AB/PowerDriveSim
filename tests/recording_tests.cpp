#include "core/editor/document.hpp"
#include "core/editor/properties.hpp"
#include "core/solver/reference/reference.hpp"
#include "formats/project/project.hpp"
#include "results/measurements.hpp"
#include <cmath>
#include <fstream>
#include <iomanip>
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
        BlockVector<int, 4> blocks;
        for (int i = 0; i < 10; ++i)
            blocks.push_back(i);
        check(blocks.size() == 10 && blocks.capacity() == 12 && blocks[4] == 4 && blocks.at(9) == 9,
              "Block history crosses storage boundaries");
        auto copied = blocks;
        copied.erase(copied.begin() + 2, copied.begin() + 5);
        check(copied.size() == 7 && copied[2] == 5 && copied.back() == 9,
              "Block history erases a random-access range");
        auto moved = std::move(blocks);
        check(blocks.empty() && moved.size() == 10 && moved.front() == 0 && moved.back() == 9,
              "Moving block history leaves a valid empty source");
        Sample inline_sample{0, {1, 2}, {true, false}};
        check(inline_sample.values.capacity() == 2 && inline_sample.gates.capacity() == 16,
              "Small recorded samples stay inline");
        inline_sample.values.push_back(3);
        check(inline_sample.values.size() == 3 && inline_sample.values[2] == 3 &&
                  inline_sample.values.capacity() >= 3,
              "Recorded samples grow beyond inline capacity");
        const Sample copied_sample = inline_sample;
        check(copied_sample.values == inline_sample.values && copied_sample.gates == inline_sample.gates,
              "Inline recorded samples copy exactly");
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
        Project signal_project=base;
        signal_project.profile.step=.001;
        signal_project.profile.stop=.004;
        CodeBlock signal_block;
        signal_block.id=derived_uuid("recording-code-block");
        signal_block.name="Recorder";
        signal_block.period=.001;
        signal_block.code="first = t; second = 2 * t; low = t < 0.002; high = t >= 0.002;";
        signal_block.outputs={{derived_uuid("recording-first"),"first","V",SignalScalarType::real,0},
                              {derived_uuid("recording-second"),"second","A",SignalScalarType::real,0},
                              {derived_uuid("recording-low"),"low","",SignalScalarType::boolean,1},
                              {derived_uuid("recording-high"),"high","",SignalScalarType::boolean,0}};
        const auto signal_plot=derived_uuid("recording-signal-plot");
        signal_project.code_blocks.push_back(signal_block);
        signal_project.plots.push_back({signal_plot,"Code outputs",0,0,4});
        for(size_t index=0;index<signal_block.outputs.size();++index)
            signal_project.wires.push_back({derived_uuid("recording-output-wire-"+std::to_string(index)),
                {signal_block.id,signal_block.outputs[index].id},{signal_plot,"in"+std::to_string(index+1)},{}});
        const std::vector<std::string> output_keys={
            signal_endpoint_key({signal_block.id,signal_block.outputs[0].id}),
            signal_endpoint_key({signal_block.id,signal_block.outputs[1].id}),
            "gate/"+signal_endpoint_key({signal_block.id,signal_block.outputs[2].id}),
            "gate/"+signal_endpoint_key({signal_block.id,signal_block.outputs[3].id})};
        check(plot_channels(signal_project,signal_plot)==output_keys,
              "Plot resolves every code-block output by its endpoint key");
        Recording signal_recording;
        signal_recording.all=false;
        signal_recording.channels=output_keys;
        const auto signal_result=execute(compile(signal_project),nullptr,nullptr,&signal_recording);
        check(signal_result.channels.size()==2&&signal_result.gate_objects.size()==2&&
                  signal_result.channels[0].object==output_keys[0]&&
                  signal_result.channels[1].object==output_keys[1]&&
                  "gate/"+signal_result.gate_objects[0]==output_keys[2]&&
                  "gate/"+signal_result.gate_objects[1]==output_keys[3],
              "Scope recording keeps distinct real and Boolean code-block outputs");
        check(signal_result.channels[0].name=="first"&&signal_result.channels[0].unit=="V"&&
                  result_channel(signal_result,2).name=="low"&&result_channel(signal_result,2).unit=="bool",
              "Code-block output metadata reaches recorded channels");
        check(std::abs(signal_result.samples.back().values[0]-.004)<1e-15&&
                  std::abs(signal_result.samples.back().values[1]-.008)<1e-15&&
                  !signal_result.samples.back().gates[0]&&signal_result.samples.back().gates[1],
              "Recorded code-block outputs contain the accepted runtime frame");
        const auto signal_empty=execute(compile(signal_project),nullptr,nullptr,&off);
        check(signal_empty.samples.empty()&&signal_empty.samples.capacity()==0&&
                  signal_empty.channels.empty()&&signal_empty.gate_objects.empty(),
              "Unsubscribed code-block outputs allocate no sample history");
        std::ostringstream v5;
        write_project(base, v5);
        std::istringstream lines(v5.str());
        std::string line;
        std::ostringstream v4;
        std::getline(lines, line);
        v4 << "PowerDriveSim 4\n";
        while (std::getline(lines, line)) {
            if (line.rfind("wire ", 0) == 0) {
                const auto legacy_suffix = line.rfind(" \"\" 2 0");
                if (legacy_suffix != std::string::npos && legacy_suffix + 7 == line.size())
                    line.erase(legacy_suffix);
            }
            if (line.rfind("scope_enabled ", 0) != 0 && line.rfind("scope_point ", 0) != 0 &&
                line.rfind("initialization ", 0) != 0 && line.rfind("stepping ", 0) != 0)
                v4 << line << '\n';
        }
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
        for(const auto& sample:pulse_result.samples){if(std::abs(sample.time-.0001)<1e-12){check(sample.gates[0],"PWM rising edge");rise=true;}if(std::abs(sample.time-(.0001+.25/1000))<1e-12){check(!sample.gates[0],"PWM falling edge");fall=true;}}
        check(rise&&fall,"PWM schedules exact edges independently of timestep");
        doc.transform({pwm},1,true);
        std::ostringstream rotated;write_project(doc.project(),rotated);std::istringstream reload(rotated.str());auto restored=read_project(reload);
        check(restored.patterns.back().pwm&&restored.patterns.back().frequency==1000&&restored.patterns.back().orientation.mirrored,"PWM and orientation roundtrip");
        auto fragment=doc.copy({pwm});auto pasted=doc.paste(fragment,40,40);
        check(pasted.size()==1&&pasted[0]!=pwm&&doc.project().patterns.back().pwm,"Copy preserves PWM and assigns independent UUID");
        doc.undo();check(doc.project().patterns.back().id==pwm,"Paste undo");
        auto script=doc.add_pattern(180,100);
        doc.apply("Gate table",[&](Project& p){p.events.push_back({.0002,script,true});});
        doc.apply("Gate script",[&](Project& p){auto& g=p.patterns.back();g.name="Script";
            p.profile.step=1e-5;
            write_property(p,script,"gate_mode",unsigned(2));
            g.code="const double base = 500; double frequency = base * 2; double duty = 1.0 / 4.0; "
                   "return pwm(frequency, duty, 100e-6);";g.script_step=1e-5;});
        check(std::get<std::vector<GateEvent>>(read_property(doc.project(),script,"events")).size()==1,
              "Switching to code preserves the inactive gate table");
        check(std::get<unsigned>(read_property(doc.project(),script,"gate_mode"))==2,"Script is exposed as one Gate mode");
        Recording scripted;scripted.all=false;scripted.channels={"gate/"+script};
        auto script_result=execute(compile(doc.project()),nullptr,nullptr,&scripted);
        bool script_rise=false,script_fall=false,previous_script=false;
        for(const auto& sample:script_result.samples){
            const bool gate=sample.gates[0];
            if(!previous_script&&gate&&std::abs(sample.time-.0001)<1e-9)script_rise=true;
            if(previous_script&&!gate&&std::abs(sample.time-(.0001+.25/1000))<1e-9)script_fall=true;
            previous_script=gate;
        }
        check(script_rise&&script_fall,"Gate script schedules reproducible PWM edges");
        doc.apply("Formatted gate script",[&](Project& p){auto& gate=p.patterns.back();
            gate.code="double duty = 0.4;\n// formatted source\nreturn pwm(1000, duty, 0);";});
        std::ostringstream scripted_roundtrip;write_project(doc.project(),scripted_roundtrip);std::istringstream scripted_reload(scripted_roundtrip.str());auto scripted_restored=read_project(scripted_reload);
        check(scripted_restored.patterns.back().script&&scripted_restored.patterns.back().code.find("// formatted source\nreturn pwm")!=std::string::npos,"Multiline gate script roundtrip");
        check(scripted_roundtrip.str().find("// formatted source")==std::string::npos,"Gate script source is encoded on one project-file line");
        std::string legacy_script=scripted_roundtrip.str();
        legacy_script.replace(0,std::string("PowerDriveSim 23").size(),"PowerDriveSim 22");
        const auto gate_begin=legacy_script.find("gate_script ");
        const auto gate_end=legacy_script.find('\n',gate_begin);
        std::ostringstream legacy_record;
        const auto& legacy_gate=doc.project().patterns.back();
        legacy_record<<"gate_script "<<std::quoted(legacy_gate.id)<<' '<<std::quoted(legacy_gate.name)<<' '
                     <<legacy_gate.x<<' '<<legacy_gate.y<<' '<<legacy_gate.initial<<' '<<legacy_gate.script_step
                     <<" \"double duty = 0.4;\n// legacy formatted source\nreturn pwm(1000, duty, 0);\"";
        legacy_script.replace(gate_begin,gate_end-gate_begin,legacy_record.str());
        std::istringstream legacy_script_input(legacy_script);const auto legacy_script_restored=read_project(legacy_script_input);
        check(legacy_script_restored.patterns.back().code.find("// legacy formatted source\nreturn pwm")!=std::string::npos,"Legacy multiline gate script recovery");
        doc.apply("Gate timing mode",[&](Project& p){write_property(p,script,"gate_mode",unsigned(0));});
        check(!doc.project().patterns.back().pwm&&!doc.project().patterns.back().script&&
                  std::get<std::vector<GateEvent>>(read_property(doc.project(),script,"events")).size()==1,
              "Gate can switch back to its preserved timing table");
        doc.apply("Dynamic gate script",[&](Project& p){auto& g=p.patterns.back();g.script=true;
            g.code="double frequency = 500 * 2; return pwm(frequency, 1.0 / 4.0, "
                   "ramp(0, 0.001, 0.0004, 0));";g.script_step=1e-5;});
        Recording dynamic;dynamic.all=false;dynamic.channels={"gate/"+script};
        auto dynamic_result=execute(compile(doc.project()),nullptr,nullptr,&dynamic);
        bool dynamic_edge=false;
        for(const auto& sample:dynamic_result.samples)
            dynamic_edge|=sample.time>0&&sample.gates[0];
        check(dynamic_edge,"Gate script PWM can use ramped parameters based on simulation time");
        doc.apply("Boolean gate script",[&](Project& p){auto& g=p.patterns.back();
            g.code="const double begin = 0.0002; const double width = 0.0003; "
                   "return t >= begin && t < begin + width;";});
        auto boolean_result=execute(compile(doc.project()),nullptr,nullptr,&dynamic);
        bool boolean_rise=false,boolean_fall=false,previous_boolean=false;
        for(const auto& sample:boolean_result.samples){
            const bool gate=sample.gates[0];
            if(!previous_boolean&&gate&&std::abs(sample.time-.0002)<1e-9)boolean_rise=true;
            if(previous_boolean&&!gate&&std::abs(sample.time-.0005)<1e-9)boolean_fall=true;
            previous_boolean=gate;
        }
        check(boolean_rise&&boolean_fall,"C-like gate script evaluates declarations, arithmetic, return and logic");
        doc.apply("Stateful C gate script",[&](Project& p){auto& g=p.patterns.back();
            g.code="double pulse(double now) { if (now >= 0.0002 && now < 0.0005) return 1; return 0; } "
                   "static int calls = 0; calls++; if (calls > 100000) return false; return pulse(t);";});
        auto c_result=execute(compile(doc.project()),nullptr,nullptr,&dynamic);
        bool c_rise=false,c_fall=false,previous_c=false;
        for(const auto& sample:c_result.samples){const bool gate=sample.gates[0];
            if(!previous_c&&gate&&std::abs(sample.time-.0002)<1e-9)c_rise=true;
            if(previous_c&&!gate&&std::abs(sample.time-.0005)<1e-9)c_fall=true;previous_c=gate;}
        check(c_rise&&c_fall,"Gate C supports functions, branches, assignments and persistent static state");
        doc.apply("Ramped C gate script",[&](Project& p){auto& g=p.patterns.back();
            g.code="double curr_ramp = ramp(0, 10, 0.008333333, 0.001111111); "
                   "return phasepwm(50, 0.02, curr_ramp) && stime == t;";
            g.script_step=1; // Legacy saved value must not control Gate execution anymore.
            p.profile.stop=.03;p.profile.step=1e-5;});
        auto ramp_result=execute(compile(doc.project()),nullptr,nullptr,&dynamic);
        bool ramp_pulse=false;
        for(const auto& sample:ramp_result.samples)ramp_pulse|=sample.gates[0];
        check(ramp_pulse,"Gate C evaluates a local ramp variable on the simulation step using stime");
        doc.apply("Pure ramped PWM",[&](Project& p){auto& g=p.patterns.back();
            g.code="double delay = ramp(0, 10, 0.008333333, 0.001111111);\n"
                   "return phasepwm(50, 0.02, delay);";
            p.profile.stop=10;p.profile.step=1e-5;});
        const auto pure_ramp_ir=compile(doc.project());
        check(pure_ramp_ir.gate_programs.size()==1,
              "Ramped Gate C always uses the same compiled runtime path");
        doc.apply("Six-output Gate C",[&](Project& p){auto& g=p.patterns.back();
            g.outputs=6;g.code="for (int ind = 0; ind < 6; ++ind) IN[ind] = ind % 2;";
            p.profile.stop=.01;p.profile.step=1e-5;});
        const auto output1=derived_uuid("gate-output:"+script+":1");
        Recording six;six.all=false;six.channels={"gate/"+script,"gate/"+output1};
        const auto six_result=execute(compile(doc.project()),nullptr,nullptr,&six);
        check(six_result.gate_objects.size()==2&&!six_result.samples.back().gates[0]&&six_result.samples.back().gates[1],
              "One Gate C program drives independently indexed outputs");
        doc.apply("Six compiled PWM outputs",[&](Project& p){auto& g=p.patterns.back();
            g.code="IN[0] = phasepwm(50, 0.02, 0);\n"
                   "IN[1] = phasepwm(50, 0.02, 0.002);\n"
                   "IN[2] = phasepwm(50, 0.02, 0.003);\n"
                   "IN[3] = phasepwm(50, 0.02, 0.004);\n"
                   "IN[4] = phasepwm(50, 0.02, 0.005);\n"
                   "IN[5] = phasepwm(50, 0.02, ramp(0, 10, 0.006, 0.001));";
            p.profile.stop=.03;});
        const auto compiled_six_ir=compile(doc.project());
        check(compiled_six_ir.gate_programs.size()==1,
              "Independent indexed PWM outputs use one compiled Gate program without an event table");
        const auto compiled_six_result=execute(compiled_six_ir,nullptr,nullptr,&six);
        check(compiled_six_result.samples.front().gates[0]&&
                  std::any_of(compiled_six_result.samples.begin(),compiled_six_result.samples.end(),
                          [](const Sample& sample){return sample.gates[0]||sample.gates[1];}),
              "Compiled indexed PWM output changes remain observable");
        std::ostringstream six_saved;write_project(doc.project(),six_saved);std::istringstream six_input(six_saved.str());
        check(read_project(six_input).patterns.back().outputs==6,"Multi-output Gate count roundtrips");
        for (const auto *name : {"bidirectional-charge", "bidirectional-discharge", "half-bridge",
                                 "full-bridge", "vsi-2l", "npc-3l", "open-end-winding",
                                 "thyristor-bridge-3p"}) {
            std::ifstream example(std::string(argv[1]) + "/examples/" + name + ".pds");
            auto periodic = read_project(example);
            periodic.profile.stop = 10;
            const auto periodic_ir = compile(periodic);
            check(!periodic_ir.gate_programs.empty() ||
                      (!periodic_ir.events.empty() && periodic_ir.events.back().time > 9.95),
                  "Periodic example keeps switching through a ten-second run");
            if (std::string(name) == "vsi-2l") {
                periodic.profile.stop = 1000;
                const auto long_ir = compile(periodic);
                check(!long_ir.gate_programs.empty() && long_ir.events.size() < 200000,
                      "Long programmable PWM runs from Gate code without a huge event table");
            }
        }
        doc.apply("Invalid gate script",[&](Project& p){p.patterns.back().outputs=1;p.patterns.back().code=
            "double a = b; double b = a; return a;";});
        bool addressed_script_error=false;
        try {
            (void)compile(doc.project());
        } catch(const Diagnostic& diagnostic) {
            addressed_script_error=diagnostic.code=="invalid_gate_script"&&diagnostic.object==script;
        }
        check(addressed_script_error,"Gate script errors address the edited gate block");
        std::cout << "PASS plots, schema5, selective recording, no-history execution, signal edges and "
                     "export subsets\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
