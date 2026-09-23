#include "core/ir/ir.hpp"
#include "core/compiler/signal.hpp"
#include "core/compiler/topology.hpp"
#include "core/model/waveform.hpp"
#include "core/model/semiconductor.hpp"
#include "core/model/connectivity.hpp"
#include "core/model/expression.hpp"
#include "core/model/c_program.hpp"
#include "core/model/hierarchy.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <sstream>
namespace pds {
namespace {
bool gate_c_value(const CProgram &program,CProgramState &state,double time) {
    const auto result=execute_c_program(program,time,&state);
    return result.return_value.value_or(0)!=0;
}
std::string gate_port(unsigned index) { return index==0?"out":"out"+std::to_string(index); }
std::string gate_signal_id(const GatePattern& gate,unsigned index) {
    return index==0?gate.id:derived_uuid("gate-output:"+gate.id+":"+std::to_string(index));
}
} // namespace

static SimulationIR compile_flat(const Project& p) {
    if(p.schema!=project_schema) throw Diagnostic("schema_version",p.id,"Unsupported schema");
    std::set<std::string> ids;
    auto check_id=[&](const std::string& id) {
        if(!valid_uuid(id) || !ids.insert(id).second) throw Diagnostic("invalid_uuid",id,"UUID is invalid or duplicated");
    };
    check_id(p.id);
    if(!std::isfinite(p.profile.stop) || !std::isfinite(p.profile.step) || p.profile.stop<=0 || p.profile.step<=0)
        throw Diagnostic("invalid_profile",p.id,"Stop time and step must be positive finite SI values");
    (void)method_name(p.profile.method);
    (void)initial_state_name(p.profile.initial_state);
    if (!std::isfinite(p.profile.warmup) || p.profile.warmup < 0 || p.profile.warmup >= p.profile.stop)
        throw Diagnostic("invalid_profile", p.id, "Warm-up must be finite, nonnegative and shorter than stop time");
    validate_step_control(p.profile, p.id);
    if(p.profile.max_iterations<1 || p.profile.max_iterations>1024
       || !std::isfinite(p.profile.voltage_tolerance) || p.profile.voltage_tolerance<=0
       || !std::isfinite(p.profile.current_tolerance) || p.profile.current_tolerance<=0
       || !std::isfinite(p.profile.relative_tolerance) || p.profile.relative_tolerance<0)
        throw Diagnostic("invalid_profile",p.id,"Nonlinear tolerances must be finite/positive; iteration budget must be 1..1024");
    SimulationIR ir; ir.project_id=p.id; ir.profile=p.profile;
    auto nodes=p.nodes; auto components=p.components;
    std::sort(nodes.begin(),nodes.end(),[](const Node& a,const Node& b){return a.id<b.id;});
    std::sort(components.begin(),components.end(),[](const Component& a,const Component& b){return a.id<b.id;});
    std::map<std::string,int> indices;
    std::set<std::string> reached;
    for(const auto& n:nodes) {
        check_id(n.id);
        indices[n.id]=n.ground?-1:ir.node_count++;
        if(n.ground) {reached.insert(n.id);ir.observations.push_back({{n.id,"u:"+n.name,"V"},-1,-1});}
        else ir.unknowns.push_back({n.id,"u:"+n.name,"V"});
    }
    if(reached.empty()) throw Diagnostic("missing_ground",p.id,"Add an electrical reference node");
    for(const auto& c:components) {
        check_id(c.id);
        (void)kind_name(c.kind);
        validate_waveform(c);
        validate_semiconductor(c);
        if(!indices.count(c.positive) || !indices.count(c.negative))
            throw Diagnostic("missing_terminal",c.id,"Connect both terminals to existing electrical nodes");
        if(c.positive==c.negative && c.kind!=Kind::voltage_probe) throw Diagnostic("shorted_component",c.id,"Both terminals reference the same node");
        if(!std::isfinite(c.value) || !std::isfinite(c.initial) || !std::isfinite(c.x) || !std::isfinite(c.y))
            throw Diagnostic("invalid_parameter",c.id,"Parameters must be finite");
        if((c.kind==Kind::resistor || c.kind==Kind::capacitor || c.kind==Kind::inductor) && c.value<=0)
            throw Diagnostic("invalid_parameter",c.id,"R, L and C must be strictly positive");
        if(c.parallel_resistance_enabled) {
            if(c.kind!=Kind::inductor)
                throw Diagnostic("invalid_parameter",c.id,"Parallel resistance is supported only for inductors");
            if(!std::isfinite(c.parallel_resistance)||c.parallel_resistance<=0)
                throw Diagnostic("invalid_parameter",c.id,"Parallel resistance must be strictly positive");
        }
        if(c.kind==Kind::diode && (c.value!=0 || c.initial!=0 || c.closed))
            throw Diagnostic("invalid_parameter",c.id,"Ideal diode uses value=0, initial=0 and closed=0; its state is solved automatically");
        if((c.kind==Kind::thyristor || c.kind==Kind::igbt) && (c.value!=0 || c.initial!=0))
            throw Diagnostic("invalid_parameter",c.id,"Controlled rectifier uses value=0 and initial=0; thyristor latch parameters are explicit");
        Stamp s{c,indices.at(c.positive),indices.at(c.negative),-1};
        if(c.kind!=Kind::resistor && c.kind!=Kind::current && c.kind!=Kind::voltage_probe) {
            s.branch=static_cast<int>(ir.unknowns.size());
            ir.unknowns.push_back({c.id,"i:"+c.name,"A"});
        }
        if(c.kind==Kind::voltage_probe)
            ir.observations.push_back({{c.id,"u:"+c.name,"V"},s.positive,s.negative});
        else ir.stamps.push_back(s);
        if(c.kind==Kind::resistor)ir.observations.push_back({{c.id,"i:"+c.name,"A"},s.positive,s.negative,1/c.value,0});
        if(c.kind==Kind::current)ir.observations.push_back({{c.id,"i:"+c.name,"A"},-1,-1,0,c.value,
            c.source.kind==Waveform::dc?-1:static_cast<int>(ir.stamps.size()-1)});
    }
    // Current sources do not establish a voltage-reference path.
    // State-dependent ideal loops/islands are diagnosed after a failed solve.
    for(size_t pass=0;pass<nodes.size();++pass)
        for(const auto& c:components) if(c.kind!=Kind::current && c.kind!=Kind::voltage_probe) {
            if(reached.count(c.positive)) reached.insert(c.negative);
            if(reached.count(c.negative)) reached.insert(c.positive);
        }
    for(const auto& n:nodes) if(!reached.count(n.id))
        throw Diagnostic("floating_node",n.id,"No structural voltage-reference path; connect an explicit reference path");
    validate_source_loops(ir);
    ir.events=p.events;
    // Validate timestamps before sorting: NaN violates strict weak ordering.
    for(const auto& e:ir.events)
        if(!std::isfinite(e.time) || e.time<0 || e.time>p.profile.stop)
            throw Diagnostic("invalid_event",e.target,"Event time must lie in the simulation interval");
    std::sort(ir.events.begin(),ir.events.end(),[](const GateEvent& a,const GateEvent& b){
        return a.time==b.time?a.target<b.target:a.time<b.time;
    });
    for(size_t i=0;i<ir.events.size();++i) {
        const auto& e=ir.events[i];
        if(!std::isfinite(e.time) || e.time<0 || e.time>p.profile.stop)
            throw Diagnostic("invalid_event",e.target,"Event time must lie in the simulation interval");
        auto it=std::find_if(components.begin(),components.end(),[&](const Component& c){return c.id==e.target;});
        if(it==components.end() || !gate_controlled(it->kind))
            throw Diagnostic("invalid_gate_target",e.target,"Gate events require a gate-controlled device");
        if(i && ir.events[i-1].time==e.time && ir.events[i-1].target==e.target)
            throw Diagnostic("conflicting_gate_events",e.target,"Only one gate assignment per switch and timestamp is allowed");
    }
    std::set<std::pair<int,int>> pattern;
    for(const auto& s:ir.stamps)
        for(int a:{s.positive,s.negative,s.branch}) for(int b:{s.positive,s.negative,s.branch})
            if(a>=0 && b>=0) pattern.emplace(a,b);
    ir.sparsity.assign(pattern.begin(),pattern.end());
    return ir;
}
static SimulationIR compile_wired(const Project& source, const std::map<std::string,ObjectPath>& origins) {
    Project project=source;
    std::set<std::string> runtime_gate_scripts;
    // Manual table edges remain in the document while another gate mode is
    // active. They are inactive input, not a second driver of the same signal.
    for(const auto& g:project.patterns)if(g.pwm||g.script)
        std::erase_if(project.events,[&](const GateEvent& event){return event.target==g.id;});
    (void)resolve_connections(project); // Validate active parameters before generating scheduled edges.
    if(!std::isfinite(project.profile.stop)||project.profile.stop<=0)throw Diagnostic("invalid_profile",project.id,"Stop time must be positive and finite");
    // A computed periodic edge may differ from the same fixed-grid instant by
    // one rounding bit. Use the exact grid timestamp to avoid a spurious tiny
    // step that records the old gate state at that instant.
    auto generated_time = [&](double time) {
        if (!std::isfinite(project.profile.step) || project.profile.step <= 0)
            return time;
        const double grid = std::round(time / project.profile.step);
        if (!std::isfinite(grid) || grid > std::ldexp(1.0, 53))
            return time;
        const double aligned = grid * project.profile.step;
        const double tolerance = 64 * std::numeric_limits<double>::epsilon() *
                                 std::max({1.0, std::abs(time), std::abs(aligned)});
        return std::abs(time - aligned) <= tolerance ? aligned : time;
    };
    size_t generated=0;
    auto generated_edge = [&](const GatePattern &g, double time, bool state, size_t &generated) {
        const double tolerance = 64 * std::numeric_limits<double>::epsilon() *
                                 std::max({1.0, std::abs(time), std::abs(project.profile.stop)});
        if(time>0&&time<=project.profile.stop+tolerance){
            if(time>project.profile.stop)time=project.profile.stop;
            time=std::min(generated_time(time),project.profile.stop);
            if(time<=0)return;
            if(++generated>1000000)throw Diagnostic("pwm_event_limit",g.id,"Generated gate signal exceeds one million edges; reduce frequency, script step or duration");
            project.events.push_back({time,g.id,state});
        }
    };
    for(auto& g:project.patterns)if(g.pwm){
        g.initial=g.delay==0&&g.duty>0;
        if(g.duty==0||g.delay>project.profile.stop)continue;
        auto edge=[&](double time,bool state){generated_edge(g,time,state,generated);};
        if(g.duty==1){edge(g.delay,true);continue;}
        double count=(project.profile.stop-g.delay)*g.frequency;
        if(!std::isfinite(count)||count>500000)throw Diagnostic("pwm_event_limit",g.id,"PWM exceeds one million edges; reduce frequency or simulation duration");
        for(size_t k=0;k<=static_cast<size_t>(std::floor(count));++k){double rise=g.delay+static_cast<double>(k)/g.frequency;double fall=g.delay+(static_cast<double>(k)+g.duty)/g.frequency;edge(rise,true);edge(fall,false);}
    }
    for(auto& g:project.patterns)if(g.script){
      try {
        CProgramOptions c_options;c_options.diagnostic_code="invalid_gate_script";c_options.object=g.id;
        c_options.allow_time=true;c_options.allow_gate_functions=true;c_options.require_return=g.outputs==1;
        if(g.outputs>1){c_options.external_arrays["IN"]=g.outputs;c_options.writable_arrays.insert("IN");}
        const auto c_program=compile_c_program(g.code,c_options);
        if(g.outputs==1) {
            CProgramState c_state;
            g.initial=gate_c_value(c_program,c_state,0);
        }
        runtime_gate_scripts.insert(g.id);
      } catch(const Diagnostic& diagnostic) {
        if(diagnostic.code=="invalid_gate_script"&&diagnostic.object.empty())
            throw Diagnostic(diagnostic.code,g.id,diagnostic.what(),diagnostic.time);
        throw;
      }
    }

    std::sort(project.events.begin(), project.events.end(), [](const GateEvent &a, const GateEvent &b) {
        return a.time == b.time ? a.target < b.target : a.time < b.time;
    });
    double simultaneous_time = 0;
    bool have_time = false;
    for (auto &event : project.events) {
        const double scale = std::max({1.0, std::abs(simultaneous_time), std::abs(event.time)});
        if (have_time && std::abs(event.time - simultaneous_time) <=
                             64 * std::numeric_limits<double>::epsilon() * scale) {
            event.time = simultaneous_time;
        } else {
            simultaneous_time = event.time;
            have_time = true;
        }
    }

    auto resolved=resolve_connections(project,origins);
    auto ir=compile_flat(resolved.project);
    auto patterns=project.patterns;std::sort(patterns.begin(),patterns.end(),[](const GatePattern& a,const GatePattern& b){return a.id<b.id;});
    for(const auto& pattern:patterns)for(unsigned index=0;index<pattern.outputs;++index) {
        const auto signal=gate_signal_id(pattern,index);
        ir.gate_signals.push_back({signal,pattern.name+(pattern.outputs>1?"["+std::to_string(index)+"]":""),pattern.initial});
    }
    for(const auto& pattern:patterns)if(runtime_gate_scripts.count(pattern.id)) {
        GateProgram program{pattern.id,pattern.code,{}};
        for(unsigned index=0;index<pattern.outputs;++index) {
            const auto signal_id=gate_signal_id(pattern,index);
            const auto signal=std::find_if(ir.gate_signals.begin(),ir.gate_signals.end(),[&](const GateSignal& candidate){return candidate.id==signal_id;});
            GateProgramOutput output{static_cast<size_t>(signal-ir.gate_signals.begin()),{}};
            const auto driver=endpoint_key({pattern.id,gate_port(index)});
            for(const auto& [target,connected]:resolved.gate_drivers)if(connected==driver) {
                const auto stamp=std::find_if(ir.stamps.begin(),ir.stamps.end(),[&](const Stamp& candidate){return candidate.component.id==target;});
                if(stamp!=ir.stamps.end())output.targets.push_back(static_cast<size_t>(stamp-ir.stamps.begin()));
            }
            program.outputs.push_back(std::move(output));
        }
        ir.gate_programs.push_back(std::move(program));
    }
    for(const auto& event:project.events)
        if(std::any_of(project.patterns.begin(),project.patterns.end(),[&](const GatePattern& p){return p.id==event.target;}))ir.events.push_back(event);
    std::sort(ir.events.begin(),ir.events.end(),[](const GateEvent& a,const GateEvent& b){return a.time==b.time?a.target<b.target:a.time<b.time;});
    return ir;
}

SimulationIR compile(const Project& source) {
    Project active=source;
    auto select_gate_modes=[](Schematic& schematic) {
        for(const auto& gate:schematic.patterns)if(gate.pwm||gate.script)
            std::erase_if(schematic.events,[&](const GateEvent& event){return event.target==gate.id;});
    };
    select_gate_modes(active);
    for(auto& definition:active.definitions)select_gate_modes(definition);
    auto resolved=resolve_parameter_expressions(active);
    auto expanded=flatten(resolved);
    try {
        if(!active.instances.empty()) {
            for(const auto& [terminal,net]:resolve_connections(expanded.project).nets) {
                auto origin=expanded.origins.find(terminal.substr(0,terminal.find('/')));
                if(origin!=expanded.origins.end())expanded.origins.try_emplace(net,origin->second);
            }
        }
        auto ir=compile_wired(expanded.project,expanded.origins);
        ir.signal=compile_signal_ir(expanded.project);
        if(!ir.signal.tasks.empty()) {
            const auto connections=resolve_connections(expanded.project,expanded.origins);
            std::set<std::string> bound_inputs;
            for(const auto& task:ir.signal.tasks)for(const auto& input:task.inputs) {
                const auto key=signal_endpoint_key(input.source);
                if(!bound_inputs.insert(key).second)continue;
                if(std::any_of(ir.signal.tasks.begin(),ir.signal.tasks.end(),[&](const SignalTaskIR& candidate) {
                    return candidate.id==input.source.object;
                }))continue;
                SignalInputBinding binding;
                binding.endpoint=input.source;binding.type=input.port.type;binding.unit=input.port.unit;
                const auto component=std::find_if(expanded.project.components.begin(),expanded.project.components.end(),
                    [&](const Component& candidate){return candidate.id==input.source.object;});
                if(component!=expanded.project.components.end()) {
                    if(component->kind==Kind::current_probe) {
                        const auto channel=std::find_if(ir.unknowns.begin(),ir.unknowns.end(),[&](const Channel& candidate) {
                            return candidate.object==component->id&&candidate.unit=="A";
                        });
                        if(channel==ir.unknowns.end())throw Diagnostic("missing_signal_source",task.id,"Current probe is absent from electrical IR");
                        binding.source=SignalInputSource::unknown;
                        binding.index=static_cast<std::size_t>(channel-ir.unknowns.begin());
                    } else {
                        const auto observation=std::find_if(ir.observations.begin(),ir.observations.end(),[&](const Observation& candidate) {
                            return candidate.channel.object==component->id&&candidate.channel.unit=="V";
                        });
                        if(observation==ir.observations.end())throw Diagnostic("missing_signal_source",task.id,"Voltage probe is absent from electrical IR");
                        binding.source=SignalInputSource::observation;
                        binding.index=static_cast<std::size_t>(observation-ir.observations.begin());
                    }
                } else {
                    const auto pattern=std::find_if(expanded.project.patterns.begin(),expanded.project.patterns.end(),
                        [&](const GatePattern& candidate){return candidate.id==input.source.object;});
                    if(pattern==expanded.project.patterns.end())throw Diagnostic("missing_signal_source",task.id,"Signal source is absent from simulation IR");
                    unsigned output=0;
                    if(input.source.port!="out")output=static_cast<unsigned>(std::stoul(input.source.port.substr(3)));
                    const auto signal_id=gate_signal_id(*pattern,output);
                    const auto gate=std::find_if(ir.gate_signals.begin(),ir.gate_signals.end(),[&](const GateSignal& candidate) {
                        return candidate.id==signal_id;
                    });
                    if(gate==ir.gate_signals.end())throw Diagnostic("missing_signal_source",task.id,"Gate source is absent from simulation IR");
                    binding.source=SignalInputSource::gate;
                    binding.index=static_cast<std::size_t>(gate-ir.gate_signals.begin());
                }
                ir.signal_inputs.push_back(std::move(binding));
            }
            for(const auto& task:ir.signal.tasks)for(const auto& output:task.outputs) {
                if(output.type!=SignalScalarType::boolean)continue;
                SignalGateBinding binding{{task.id,output.id},{}};
                const auto driver=endpoint_key({task.id,output.id});
                for(const auto& [target,connected]:connections.gate_drivers)if(connected==driver) {
                    const auto stamp=std::find_if(ir.stamps.begin(),ir.stamps.end(),[&](const Stamp& candidate) {
                        return candidate.component.id==target;
                    });
                    if(stamp!=ir.stamps.end()) {
                        const auto index=static_cast<std::size_t>(stamp-ir.stamps.begin());
                        binding.targets.push_back(index);
                        ir.stamps[index].component.closed=output.initial!=0;
                    }
                }
                if(!binding.targets.empty())ir.signal_gates.push_back(std::move(binding));
            }
        }
        ir.origins=std::move(expanded.origins);
        return ir;
    } catch(Diagnostic& error) {
        if(auto origin=expanded.origins.find(error.object);origin!=expanded.origins.end()) {
            error.object=origin->second.object;error.path=origin->second.instances;
        }
        throw;
    }
}

}
