#include "core/ir/ir.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
namespace pds {
SimulationIR compile(const Project& p) {
    if(p.schema!=1) throw Diagnostic("schema_version",p.id,"Unsupported schema");
    std::set<std::string> ids;
    auto check_id=[&](const std::string& id) {
        if(!valid_uuid(id) || !ids.insert(id).second) throw Diagnostic("invalid_uuid",id,"UUID is invalid or duplicated");
    };
    check_id(p.id);
    if(!std::isfinite(p.profile.stop) || !std::isfinite(p.profile.step) || p.profile.stop<=0 || p.profile.step<=0)
        throw Diagnostic("invalid_profile",p.id,"Stop time and step must be positive finite SI values");
    SimulationIR ir; ir.project_id=p.id; ir.profile=p.profile;
    auto nodes=p.nodes; auto components=p.components;
    std::sort(nodes.begin(),nodes.end(),[](const Node& a,const Node& b){return a.id<b.id;});
    std::sort(components.begin(),components.end(),[](const Component& a,const Component& b){return a.id<b.id;});
    std::map<std::string,int> indices;
    std::set<std::string> reached;
    for(const auto& n:nodes) {
        check_id(n.id);
        indices[n.id]=n.ground?-1:ir.node_count++;
        if(n.ground) reached.insert(n.id);
        else ir.unknowns.push_back({n.id,"u:"+n.name,"V"});
    }
    if(reached.empty()) throw Diagnostic("missing_ground",p.id,"Add an electrical reference node");
    for(const auto& c:components) {
        check_id(c.id);
        (void)kind_name(c.kind);
        if(!indices.count(c.positive) || !indices.count(c.negative))
            throw Diagnostic("missing_terminal",c.id,"Connect both terminals to existing electrical nodes");
        if(c.positive==c.negative) throw Diagnostic("shorted_component",c.id,"Both terminals reference the same node");
        if(!std::isfinite(c.value) || !std::isfinite(c.initial) || !std::isfinite(c.x) || !std::isfinite(c.y))
            throw Diagnostic("invalid_parameter",c.id,"Parameters must be finite");
        if((c.kind==Kind::resistor || c.kind==Kind::capacitor || c.kind==Kind::inductor) && c.value<=0)
            throw Diagnostic("invalid_parameter",c.id,"R, L and C must be strictly positive");
        Stamp s{c,indices.at(c.positive),indices.at(c.negative),-1};
        if(c.kind!=Kind::resistor && c.kind!=Kind::current) {
            s.branch=static_cast<int>(ir.unknowns.size());
            ir.unknowns.push_back({c.id,"i:"+c.name,"A"});
        }
        ir.stamps.push_back(s);
    }
    // Current sources do not establish a voltage-reference path.
    // State-dependent ideal loops/islands are also checked by factorization.
    for(size_t pass=0;pass<nodes.size();++pass)
        for(const auto& c:components) if(c.kind!=Kind::current) {
            if(reached.count(c.positive)) reached.insert(c.negative);
            if(reached.count(c.negative)) reached.insert(c.positive);
        }
    for(const auto& n:nodes) if(!reached.count(n.id))
        throw Diagnostic("floating_node",n.id,"No structural voltage-reference path; connect an explicit reference path");
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
}