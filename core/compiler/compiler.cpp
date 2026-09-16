#include "core/ir/ir.hpp"
#include "core/compiler/topology.hpp"
#include "core/model/waveform.hpp"
#include "core/model/connectivity.hpp"
#include "core/model/hierarchy.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
namespace pds {
static SimulationIR compile_flat(const Project& p) {
    if(p.schema!=8) throw Diagnostic("schema_version",p.id,"Unsupported schema");
    std::set<std::string> ids;
    auto check_id=[&](const std::string& id) {
        if(!valid_uuid(id) || !ids.insert(id).second) throw Diagnostic("invalid_uuid",id,"UUID is invalid or duplicated");
    };
    check_id(p.id);
    if(!std::isfinite(p.profile.stop) || !std::isfinite(p.profile.step) || p.profile.stop<=0 || p.profile.step<=0)
        throw Diagnostic("invalid_profile",p.id,"Stop time and step must be positive finite SI values");
    (void)method_name(p.profile.method);
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
        if(!indices.count(c.positive) || !indices.count(c.negative))
            throw Diagnostic("missing_terminal",c.id,"Connect both terminals to existing electrical nodes");
        if(c.positive==c.negative && c.kind!=Kind::voltage_probe) throw Diagnostic("shorted_component",c.id,"Both terminals reference the same node");
        if(!std::isfinite(c.value) || !std::isfinite(c.initial) || !std::isfinite(c.x) || !std::isfinite(c.y))
            throw Diagnostic("invalid_parameter",c.id,"Parameters must be finite");
        if((c.kind==Kind::resistor || c.kind==Kind::capacitor || c.kind==Kind::inductor) && c.value<=0)
            throw Diagnostic("invalid_parameter",c.id,"R, L and C must be strictly positive");
        if(c.kind==Kind::diode && (c.value!=0 || c.initial!=0 || c.closed))
            throw Diagnostic("invalid_parameter",c.id,"Ideal diode uses value=0, initial=0 and closed=0; its state is solved automatically");
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
        if(it==components.end() || it->kind!=Kind::ideal_switch)
            throw Diagnostic("invalid_gate_target",e.target,"Gate events require an ideal switch");
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
static SimulationIR compile_wired(const Project& source) {
    Project project=source;
    (void)resolve_connections(project); // Validate parameters before generating scheduled edges.
    if(!std::isfinite(project.profile.stop)||project.profile.stop<=0)throw Diagnostic("invalid_profile",project.id,"Stop time must be positive and finite");
    size_t generated=0;
    for(auto& g:project.patterns)if(g.pwm){
        if(std::any_of(project.events.begin(),project.events.end(),[&](const GateEvent& e){return e.target==g.id;}))throw Diagnostic("conflicting_gate_events",g.id,"PWM cannot have manually recorded events");
        g.initial=g.delay==0&&g.duty>0;
        if(g.duty==0||g.delay>project.profile.stop)continue;
        auto edge=[&](double time,bool state){if(time>0&&time<=project.profile.stop){if(++generated>1000000)throw Diagnostic("pwm_event_limit",g.id,"PWM exceeds one million edges; reduce frequency or simulation duration");project.events.push_back({time,g.id,state});}};
        if(g.duty==1){edge(g.delay,true);continue;}
        double count=(project.profile.stop-g.delay)*g.frequency;
        if(!std::isfinite(count)||count>500000)throw Diagnostic("pwm_event_limit",g.id,"PWM exceeds one million edges; reduce frequency or simulation duration");
        for(size_t k=0;k<=static_cast<size_t>(std::floor(count));++k){double rise=g.delay+static_cast<double>(k)/g.frequency;double fall=g.delay+(static_cast<double>(k)+g.duty)/g.frequency;edge(rise,true);edge(fall,false);}
    }

    auto ir=compile_flat(resolve_connections(project).project);
    auto patterns=project.patterns;std::sort(patterns.begin(),patterns.end(),[](const GatePattern& a,const GatePattern& b){return a.id<b.id;});
    for(const auto& pattern:patterns)ir.gate_signals.push_back({pattern.id,pattern.name,pattern.initial});
    for(const auto& event:project.events)
        if(std::any_of(project.patterns.begin(),project.patterns.end(),[&](const GatePattern& p){return p.id==event.target;}))ir.events.push_back(event);
    std::sort(ir.events.begin(),ir.events.end(),[](const GateEvent& a,const GateEvent& b){return a.time==b.time?a.target<b.target:a.time<b.time;});
    return ir;
}

SimulationIR compile(const Project& source) {
    auto expanded=flatten(source);
    try {
        if(!source.instances.empty()) {
            for(const auto& [terminal,net]:resolve_connections(expanded.project).nets) {
                auto origin=expanded.origins.find(terminal.substr(0,terminal.find('/')));
                if(origin!=expanded.origins.end())expanded.origins.try_emplace(net,origin->second);
            }
        }
        auto ir=compile_wired(expanded.project);
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
