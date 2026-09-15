#include "core/model/connectivity.hpp"
#include <algorithm>
#include <cmath>
#include <set>
namespace pds {
std::string endpoint_key(const Endpoint& e) { return e.object+"/"+e.port; }
PortType port_type(const Project& p,const Endpoint& e) {
    for(const auto& n:p.nodes) if(n.id==e.object && e.port=="node") return {Domain::electrical,Direction::conserving};
    for(const auto& c:p.components) if(c.id==e.object) {
        if(e.port=="p" || e.port=="n") return {Domain::electrical,Direction::conserving};
        if(c.kind==Kind::ideal_switch && e.port=="gate") return {Domain::gate,Direction::input};
        if((c.kind==Kind::voltage_probe || c.kind==Kind::current_probe) && e.port=="out") return {Domain::signal,Direction::output};
    }
    for(const auto& g:p.patterns) if(g.id==e.object && e.port=="out") return {Domain::gate,Direction::output};
    throw Diagnostic("missing_port",e.object,"Port does not exist: "+e.port);
}
void validate_wire(const Project& p,const Wire& w) {
    auto a=port_type(p,w.from),b=port_type(p,w.to);
    if(w.from==w.to) throw Diagnostic("invalid_connection",w.id,"Cannot connect a port to itself");
    if(a.domain!=b.domain || (a.domain!=Domain::electrical && a.direction==b.direction))
        throw Diagnostic("incompatible_port",w.id,"Connect electrical terminals together or a matching output to an input");
    for(const auto& point:w.bends)
        if(!std::isfinite(point.x)||!std::isfinite(point.y)) throw Diagnostic("invalid_geometry",w.id,"Wire points must be finite");
}
ResolvedGraph resolve_connections(const Project& source) {
    if(!source.wired) {
        if(!source.wires.empty() || !source.patterns.empty()) throw Diagnostic("invalid_wiring",source.id,"Wire records require wired mode");
        return {source,{}};
    }
    Project p=source;
    std::map<std::string,std::string> parents;
    std::map<std::string,std::string> labels;
    std::set<std::string> ids;
    auto uuid=[&](const std::string& id) {
        if(!valid_uuid(id)||!ids.insert(id).second) throw Diagnostic("invalid_uuid",id,"Invalid or duplicate object UUID");
    };
    uuid(p.id);
    auto add=[&](const Endpoint& e,const std::string& name) { auto k=endpoint_key(e); parents[k]=k; labels[k]=name; };
    for(const auto& n:p.nodes) {
        uuid(n.id); add({n.id,"node"},n.name);
        if(!std::isfinite(n.x)||!std::isfinite(n.y)) throw Diagnostic("invalid_geometry",n.id,"Node position must be finite");
    }
    for(const auto& c:p.components) { uuid(c.id); add({c.id,"p"},c.name+".p"); add({c.id,"n"},c.name+".n"); }
    for(const auto& g:p.patterns) {
        uuid(g.id);
        if(!std::isfinite(g.x)||!std::isfinite(g.y)) throw Diagnostic("invalid_geometry",g.id,"Pattern position must be finite");
    }
    auto root=[&](std::string k) {
        while(parents.at(k)!=k) { parents[k]=parents.at(parents.at(k)); k=parents.at(k); } return k;
    };
    auto join=[&](const std::string& a,const std::string& b) {
        auto ra=root(a),rb=root(b); if(ra!=rb) parents[std::max(ra,rb)]=std::min(ra,rb);
    };
    // All reference symbols represent the same zero-potential net.
    std::string ground;
    for(const auto& n:p.nodes) if(n.ground) {
        auto k=endpoint_key({n.id,"node"}); if(ground.empty()) ground=k; else join(ground,k);
    }
    std::map<std::string,std::string> drivers;
    std::set<std::pair<std::string,std::string>> pairs;
    for(const auto& w:p.wires) {
        uuid(w.id); validate_wire(p,w);
        auto a=endpoint_key(w.from),b=endpoint_key(w.to);
        if(!pairs.insert(std::minmax(a,b)).second) throw Diagnostic("duplicate_connection",w.id,"These ports are already connected");
        if(port_type(p,w.from).domain==Domain::electrical) join(a,b);
        else {
            const auto& input=port_type(p,w.from).direction==Direction::input?w.from:w.to;
            const auto& output=port_type(p,w.from).direction==Direction::output?w.from:w.to;
            if(!drivers.emplace(input.object,output.object).second)
                throw Diagnostic("multiple_gate_drivers",input.object,"A gate input accepts exactly one driver");
        }
    }
    ResolvedGraph result;
    std::map<std::string,Node> nets;
    // Preserve named node UUIDs, choosing a stable representative if wires merge them.
    auto nodes=p.nodes; std::sort(nodes.begin(),nodes.end(),[](const Node& a,const Node& b){return a.id<b.id;});
    for(const auto& n:nodes) {
        auto r=root(endpoint_key({n.id,"node"}));
        if(!nets.count(r)) nets[r]=n;
        else nets[r].ground=nets[r].ground||n.ground;
    }
    for(const auto& [key,parent]:parents) {
        (void)parent; auto r=root(key);
        if(!nets.count(r)) nets[r]={derived_uuid("net:"+r),labels.at(r),false};
        result.nets[key]=nets.at(r).id;
    }
    p.nodes.clear();
    for(const auto& [key,node]:nets) { (void)key; p.nodes.push_back(node); }
    for(auto& c:p.components) {
        c.positive=result.nets.at(endpoint_key({c.id,"p"}));
        c.negative=result.nets.at(endpoint_key({c.id,"n"}));
        auto driver=drivers.find(c.id);
        if(driver!=drivers.end()) {
            const auto g=std::find_if(p.patterns.begin(),p.patterns.end(),[&](const GatePattern& pattern){return pattern.id==driver->second;});
            c.closed=g->initial;
        }
    }
    std::vector<GateEvent> events;
    for(const auto& event:source.events) {
        auto pattern=std::find_if(p.patterns.begin(),p.patterns.end(),[&](const GatePattern& g){return g.id==event.target;});
        if(pattern==p.patterns.end()) {
            if(drivers.count(event.target)) throw Diagnostic("multiple_gate_drivers",event.target,"Direct events conflict with a connected pattern");
            events.push_back(event);
        } else {
            if(!std::isfinite(event.time)||event.time<0||event.time>p.profile.stop)
                throw Diagnostic("invalid_event",event.target,"Pattern edge is outside the simulation interval");
            for(const auto& [target,driver]:drivers) if(driver==event.target) events.push_back({event.time,target,event.closed});
        }
    }
    p.events=std::move(events); p.wired=false; p.wires.clear(); p.patterns.clear();
    result.project=std::move(p);
    return result;
}
Project make_wired(const Project& source) {
    if(source.wired) return source;
    Project p=source; p.wired=true;
    std::map<std::string,std::vector<Point>> terminals;
    for(auto& c:p.components) {
        for(const auto& port:{std::string("p"),std::string("n")}) {
            auto node=port=="p"?c.positive:c.negative;
            if(node.empty()) continue;
            p.wires.push_back({derived_uuid("wire:"+c.id+"/"+port),{c.id,port},{node,"node"},{}});
            terminals[node].push_back({c.x+(port=="p"?-60:60),c.y});
        }
        c.positive.clear(); c.negative.clear();
    }
    for(auto& n:p.nodes) {
        const auto& points=terminals[n.id];
        if(!points.empty()) {
            n.x=points.front().x; n.y=points.front().y+(n.ground?130:-90);
        }
    }
    (void)resolve_connections(p); return p;
}
}
