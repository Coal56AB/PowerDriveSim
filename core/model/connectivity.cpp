#include "core/model/connectivity.hpp"
#include "core/model/hierarchy.hpp"
#include <algorithm>
#include <cmath>
#include <set>
namespace pds {
namespace {
bool plot_input(const Project& catalog,const Schematic& level,const Endpoint& endpoint,unsigned depth=0) {
    if(depth>64)return false;
    for(const auto& plot:level.plots)if(plot.id==endpoint.object)
        for(unsigned input=1;input<=plot.inputs;++input)
            if(endpoint.port==(plot.differential?"p":"in")+std::to_string(input)||
               (plot.differential&&endpoint.port=="n"+std::to_string(input)))return true;
    for(const auto& instance:level.instances)if(instance.id==endpoint.object) {
        const auto& body=definition(catalog,instance.definition);
        for(const auto& port:body.ports)if(port.id==endpoint.port)return plot_input(catalog,body,port.terminal,depth+1);
    }
    return false;
}
const std::string& tag_connection_name(const ConnectionTag& tag) {
    return tag.connection_name.empty()?tag.name:tag.connection_name;
}
bool ancestor_path(const std::vector<std::string>& older,const std::vector<std::string>& younger) {
    return older.size()<=younger.size()&&std::equal(older.begin(),older.end(),younger.begin());
}
bool compatible_tags(const ConnectionTag& a,const ConnectionTag& b) {
    if(a.domain!=b.domain||tag_connection_name(a)!=tag_connection_name(b))return false;
    if(a.scope==TagScope::global||b.scope==TagScope::global)return true;
    if(a.scope_path==b.scope_path)return true;
    return (a.scope==TagScope::ancestors&&ancestor_path(b.scope_path,a.scope_path))||
           (b.scope==TagScope::ancestors&&ancestor_path(a.scope_path,b.scope_path));
}
}
std::string endpoint_key(const Endpoint& e) { return e.object+"/"+e.port; }
namespace {
std::string gate_port(unsigned index) { return index==0?"out":"out"+std::to_string(index); }
std::string endpoint_object(const std::string& key) { const auto slash=key.find('/');return key.substr(0,slash); }
}
PortType port_type(const Project& p,const Endpoint& e) {
    for(const auto& i:p.instances)if(i.id==e.object) {
        for(const auto& port:definition(p,i.definition).ports)if(port.id==e.port)return {port.domain,port.direction};
    }
    for(const auto& n:p.nodes) if(n.id==e.object && e.port=="node") return {Domain::electrical,Direction::conserving};
    for(const auto& c:p.components) if(c.id==e.object) {
        if(e.port=="p" || e.port=="n") return {Domain::electrical,Direction::conserving};
        if(gate_controlled(c.kind) && e.port=="gate") return {Domain::gate,Direction::input};
        if((c.kind==Kind::voltage_probe || c.kind==Kind::current_probe) && e.port=="out") return {Domain::signal,Direction::output};
    }
    for(const auto& g:p.patterns) if(g.id==e.object)
        for(unsigned index=0;index<g.outputs;++index)if(e.port==gate_port(index))return {Domain::gate,Direction::output};
    for(const auto& t:p.tags) if(t.id==e.object && e.port=="io") return {t.domain,Direction::conserving};
    for(const auto& plot:p.plots)if(plot.id==e.object)
        for(unsigned i=1;i<=plot.inputs;++i)
            if(e.port==(plot.differential?"p":"in")+std::to_string(i)||
               (plot.differential&&e.port=="n"+std::to_string(i)))return {Domain::signal,Direction::input};
    for(const auto& block:p.code_blocks)if(block.id==e.object) {
        for(const auto& input:block.inputs)if(input.id==e.port)
            return {input.type==SignalScalarType::boolean?Domain::gate:Domain::signal,Direction::input};
        for(const auto& output:block.outputs)if(output.id==e.port)
            return {output.type==SignalScalarType::boolean?Domain::gate:Domain::signal,Direction::output};
    }
    throw Diagnostic("missing_port",e.object,"Port does not exist: "+e.port);
}
void validate_wire(const Project& p,const Wire& w) {
    auto a=port_type(p,w.from),b=port_type(p,w.to);
    if(w.from==w.to) throw Diagnostic("invalid_connection",w.id,"Cannot connect a port to itself");
    auto tag=[&](const Endpoint& e){return std::any_of(p.tags.begin(),p.tags.end(),[&](const auto& t){return t.id==e.object&&e.port=="io";});};
    bool gate_to_plot=((a.domain==Domain::gate||a.domain==Domain::electrical)&&plot_input(p,p,w.to))||((b.domain==Domain::gate||b.domain==Domain::electrical)&&plot_input(p,p,w.from));
    if((a.domain!=b.domain&&!gate_to_plot) || (a.domain!=Domain::electrical && !tag(w.from) && !tag(w.to) && a.direction==b.direction))
        throw Diagnostic("incompatible_port",w.id,"Connect electrical terminals together or a matching output to an input");
    for(const auto& point:w.bends)
        if(!std::isfinite(point.x)||!std::isfinite(point.y)) throw Diagnostic("invalid_geometry",w.id,"Wire points must be finite");
}
ResolvedGraph resolve_connections(const Project& source, const std::map<std::string,ObjectPath>& origins) {
    if(!source.instances.empty()) {
        auto expanded=flatten(source);
        auto resolved=resolve_connections(expanded.project,expanded.origins);
        for(const auto& [key,terminal]:expanded.terminals) {
            auto net=resolved.nets.find(endpoint_key(terminal));
            if(net!=resolved.nets.end())resolved.nets[key]=net->second;
        }
        return resolved;
    }
    if(!source.wired) {
        if(!source.wires.empty() || !source.tags.empty() || !source.patterns.empty() || !source.plots.empty()) throw Diagnostic("invalid_wiring",source.id,"Wire records require wired mode");
        return {source,{}};
    }
    Project p=source;
    std::map<std::string,std::string> parents;
    std::map<std::string,std::string> labels;
    std::set<std::string> electrical_endpoints;
    std::set<std::string> ids;
    auto uuid=[&](const std::string& id) {
        if(!valid_uuid(id)||!ids.insert(id).second) throw Diagnostic("invalid_uuid",id,"Invalid or duplicate object UUID");
    };
    uuid(p.id);
    auto add=[&](const Endpoint& e,const std::string& name,bool electrical=true) {
        auto k=endpoint_key(e); parents[k]=k; labels[k]=name;
        if(electrical)electrical_endpoints.insert(k);
    };
    for(const auto& n:p.nodes) {
        uuid(n.id); add({n.id,"node"},n.name);
        if(!std::isfinite(n.x)||!std::isfinite(n.y)) throw Diagnostic("invalid_geometry",n.id,"Node position must be finite");
    }
    for(const auto& c:p.components) { uuid(c.id); add({c.id,"p"},c.name+".p"); add({c.id,"n"},c.name+".n"); }
    for(const auto& t:p.tags) {
        uuid(t.id);
        if(t.name.empty())throw Diagnostic("invalid_tag",t.id,"Connection tag name must not be empty");
        if(unsigned(t.scope)>unsigned(TagScope::global))throw Diagnostic("invalid_tag",t.id,"Unknown tag scope");
        if(!std::isfinite(t.x)||!std::isfinite(t.y)) throw Diagnostic("invalid_geometry",t.id,"Tag position must be finite");
        add({t.id,"io"},t.name,t.domain==Domain::electrical);
    }
    for(const auto& g:p.patterns) {
        uuid(g.id);
        if(g.outputs<1||g.outputs>16)throw Diagnostic("invalid_gate_outputs",g.id,"Gate requires 1..16 outputs");
        if(g.outputs>1&&!g.script)throw Diagnostic("invalid_gate_outputs",g.id,"Multiple Gate outputs require C code mode");
        if(g.pwm&&(!std::isfinite(g.frequency)||g.frequency<=0||!std::isfinite(g.duty)||g.duty<0||g.duty>1||!std::isfinite(g.delay)||g.delay<0))throw Diagnostic("invalid_pwm",g.id,"PWM frequency must be positive, duty must be 0..1 and delay non-negative");
        if(g.script&&g.code.empty())throw Diagnostic("invalid_gate_script",g.id,"Gate script requires code");
        if(g.pwm&&g.script)throw Diagnostic("invalid_gate_script",g.id,"Gate script and PWM modes are mutually exclusive");
        if(!std::isfinite(g.x)||!std::isfinite(g.y)) throw Diagnostic("invalid_geometry",g.id,"Pattern position must be finite");
    }
    for(const auto& plot:p.plots){
        uuid(plot.id);
        if(!std::isfinite(plot.x)||!std::isfinite(plot.y)||plot.inputs<1||plot.inputs>(plot.differential?8u:16u))
            throw Diagnostic("invalid_plot",plot.id,"A plot requires finite coordinates and 1..16 inputs");
    }
    auto root=[&](std::string k) {
        while(parents.at(k)!=k) { parents[k]=parents.at(parents.at(k)); k=parents.at(k); } return k;
    };
    auto join=[&](const std::string& a,const std::string& b) {
        auto ra=root(a),rb=root(b); if(ra!=rb) parents[std::max(ra,rb)]=std::min(ra,rb);
    };
    // Reference symbols are global only within the same explicit name. This
    // allows independent GND1/GND2 domains while repeated GND1 symbols remain
    // one net without a drawn wire.
    std::map<std::string, std::string> grounds;
    for(const auto& n:p.nodes) if(n.ground) {
        if(n.name.empty())throw Diagnostic("invalid_ground",n.id,"Ground name must not be empty");
        auto k=endpoint_key({n.id,"node"});
        auto [found,inserted]=grounds.emplace(n.name,k);
        if(!inserted)join(found->second,k);
    }
    for(const auto& tag:p.tags)
        for(const auto& other:p.tags) if(other.id>tag.id&&compatible_tags(tag,other))
            join(endpoint_key({tag.id,"io"}),endpoint_key({other.id,"io"}));
    std::map<std::string,std::string> drivers, tag_drivers;
    std::map<std::string,std::vector<std::string>> tag_inputs;
    std::set<std::string> plot_inputs;
    std::set<std::pair<std::string,std::string>> pairs;
    for(const auto& w:p.wires) {
        uuid(w.id); validate_wire(p,w);
        auto a=endpoint_key(w.from),b=endpoint_key(w.to);
        if(!pairs.insert(std::minmax(a,b)).second) throw Diagnostic("duplicate_connection",w.id,"These ports are already connected");
        auto is_plot=[&](const Endpoint& e){return std::any_of(p.plots.begin(),p.plots.end(),[&](const PlotBlock& plot){return plot.id==e.object;});};
        auto find_tag=[&](const Endpoint& e)->const ConnectionTag*{
            auto it=std::find_if(p.tags.begin(),p.tags.end(),[&](const auto& tag){return tag.id==e.object&&e.port=="io";});
            return it==p.tags.end()?nullptr:&*it;
        };
        if(is_plot(w.from)||is_plot(w.to)){
            const auto& input=is_plot(w.from)?w.from:w.to;
            if(!plot_inputs.insert(endpoint_key(input)).second)throw Diagnostic("multiple_plot_drivers",input.object,"Each plot input accepts one signal");
            continue; // A plot tap observes a net; it is never an electrical connection.
        }
        if(port_type(p,w.from).domain==Domain::electrical) join(a,b);
        else {
            if(auto* tag=find_tag(w.from)?find_tag(w.from):find_tag(w.to)) {
                const auto& other=find_tag(w.from)?w.to:w.from;
                const auto type=port_type(p,other);
                const auto key=root(endpoint_key({tag->id,"io"}));
                if(type.direction==Direction::output) {
                    if(!tag_drivers.emplace(key,endpoint_key(other)).second)
                        throw Diagnostic("multiple_gate_drivers",tag->id,"A tag group accepts exactly one driver");
                } else if(type.direction==Direction::input) tag_inputs[key].push_back(other.object);
                else throw Diagnostic("incompatible_port",w.id,"Non-electrical tags connect outputs to inputs");
                continue;
            }
            const auto& input=port_type(p,w.from).direction==Direction::input?w.from:w.to;
            const auto& output=port_type(p,w.from).direction==Direction::output?w.from:w.to;
            bool plot=std::any_of(p.plots.begin(),p.plots.end(),[&](const PlotBlock& g){return g.id==input.object;});
            if(plot){
                if(!plot_inputs.insert(endpoint_key(input)).second)throw Diagnostic("multiple_plot_drivers",input.object,"Each plot input accepts one signal");
                continue;
            }
            if(!drivers.emplace(input.object,endpoint_key(output)).second)
                throw Diagnostic("multiple_gate_drivers",input.object,"A gate input accepts exactly one driver");
        }
    }
    for(const auto& [key,driver]:tag_drivers)
        for(const auto& input:tag_inputs[key])
            if(!drivers.emplace(input,driver).second)
                throw Diagnostic("multiple_gate_drivers",input,"A gate input accepts exactly one driver");
    ResolvedGraph result;
    result.gate_drivers=drivers;
    std::map<std::string,Node> nets;
    // Preserve named node UUIDs, choosing a stable representative if wires merge them.
    auto nodes=p.nodes; std::sort(nodes.begin(),nodes.end(),[](const Node& a,const Node& b){return a.id<b.id;});
    for(const auto& n:nodes) {
        auto r=root(endpoint_key({n.id,"node"}));
        if(!nets.count(r)) nets[r]=n;
        else nets[r].ground=nets[r].ground||n.ground;
    }
    std::map<std::string,std::pair<size_t,std::string>> names;
    for(const auto& n:nodes) if(!n.name.empty()) {
        const auto r=root(endpoint_key({n.id,"node"}));
        const auto origin=origins.find(n.id);
        const auto rank=std::make_pair(origin==origins.end()?size_t(0):origin->second.instances.size(),n.id);
        if(!names.count(r)||rank<names.at(r)) {
            names[r]=rank;
            nets.at(r).name=n.name;
        }
    }
    for(const auto& [key,parent]:parents) {
        (void)parent;
        if(!electrical_endpoints.count(key))continue;
        auto r=root(key);
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
            const auto object=endpoint_object(driver->second);
            const auto g=std::find_if(p.patterns.begin(),p.patterns.end(),[&](const GatePattern& pattern){return pattern.id==object;});
            c.closed=g->initial;
        }
    }
    std::vector<GateEvent> events;
    std::set<std::pair<std::string,double>> pattern_times;
    for(const auto& event:source.events) {
        auto pattern=std::find_if(p.patterns.begin(),p.patterns.end(),[&](const GatePattern& g){return g.id==event.target;});
        if(pattern==p.patterns.end()) {
            if(drivers.count(event.target)) throw Diagnostic("multiple_gate_drivers",event.target,"Direct events conflict with a connected pattern");
            events.push_back(event);
        } else {
            if(!std::isfinite(event.time)||event.time<0||event.time>p.profile.stop)
                throw Diagnostic("invalid_event",event.target,"Pattern edge is outside the simulation interval");
            if(!pattern_times.insert({event.target,event.time}).second)
                throw Diagnostic("conflicting_gate_events",event.target,"Only one assignment per pattern and timestamp is allowed");
            for(const auto& [target,driver]:drivers) if(driver==endpoint_key({event.target,"out"})) events.push_back({event.time,target,event.closed});
        }
    }
    p.events=std::move(events); p.wired=false; p.wires.clear(); p.tags.clear(); p.patterns.clear(); p.plots.clear();
    result.project=std::move(p);
    return result;
}
namespace { std::string plot_source(const Project& p,const Endpoint& input); }
std::vector<std::string> plot_channels(const Project& p,const std::string& id){
    if(!p.instances.empty())return plot_channels(flatten(p).project,id);
    std::vector<std::string> result;
    auto plot=std::find_if(p.plots.begin(),p.plots.end(),[&](const PlotBlock& g){return g.id==id;});
    if(plot==p.plots.end())return result;
    if(plot->differential) {
        const auto pairs=plot_differential_channels(p,id);
        for(unsigned i=1;i<=plot->inputs;++i) {
            if(i<=pairs.size()&&!pairs[i-1].first.empty()&&!pairs[i-1].second.empty())
                result.push_back("diff/"+id+"/"+std::to_string(i));
        }
        return result;
    }
    for(unsigned i=1;i<=plot->inputs;++i){
        Endpoint input{id,"in"+std::to_string(i)};
        const auto key=plot_source(p,input);
        if(!key.empty()&&std::find(result.begin(),result.end(),key)==result.end())result.push_back(key);
    }return result;
}
namespace {
std::string tagged_source(const Project& p,const ConnectionTag& source) {
    for(const auto& tag:p.tags) {
        if(!compatible_tags(source,tag))continue;
        const Endpoint terminal{tag.id,"io"};
        for(const auto& wire:p.wires) {
            const Endpoint* other=wire.from==terminal?&wire.to:(wire.to==terminal?&wire.from:nullptr);
            if(!other)continue;
            if(std::any_of(p.tags.begin(),p.tags.end(),[&](const ConnectionTag& candidate){
                   return candidate.id==other->object&&other->port=="io";
               }))continue;
            const auto type=port_type(p,*other);
            if(type.direction==Direction::output)
                return type.domain==Domain::gate?"gate/"+other->object:other->object;
        }
    }
    return {};
}
std::string plot_source(const Project& p,const Endpoint& input) {
    for(const auto& w:p.wires) {
        const Endpoint* source=w.to==input?&w.from:(w.from==input?&w.to:nullptr);
        if(!source)continue;
        const auto domain=port_type(p,*source).domain;
        if(auto tag=std::find_if(p.tags.begin(),p.tags.end(),[&](const ConnectionTag& candidate){
               return candidate.id==source->object&&source->port=="io";
           });tag!=p.tags.end()) {
            if(domain==Domain::electrical)return resolve_connections(p).nets.at(endpoint_key(*source));
            return tagged_source(p,*tag);
        }
        if(domain==Domain::gate)return "gate/"+source->object;
        if(domain==Domain::electrical)return resolve_connections(p).nets.at(endpoint_key(*source));
        return source->object;
    }
    return {};
}
}
std::vector<std::pair<std::string,std::string>> plot_differential_channels(const Project& p,
                                                                           const std::string& id) {
    if(!p.instances.empty())return plot_differential_channels(flatten(p).project,id);
    std::vector<std::pair<std::string,std::string>> result;
    const auto plot=std::find_if(p.plots.begin(),p.plots.end(),[&](const PlotBlock& g){return g.id==id;});
    if(plot==p.plots.end()||!plot->differential)return result;
    for(unsigned i=1;i<=plot->inputs;++i)
        result.emplace_back(plot_source(p,{id,"p"+std::to_string(i)}),
                            plot_source(p,{id,"n"+std::to_string(i)}));
    return result;
}
std::vector<std::string> plot_source_channels(const Project& p,const std::string& id) {
    if(!p.instances.empty())return plot_source_channels(flatten(p).project,id);
    const auto plot=std::find_if(p.plots.begin(),p.plots.end(),[&](const PlotBlock& g){return g.id==id;});
    if(plot==p.plots.end())return {};
    if(!plot->differential)return plot_channels(p,id);
    std::vector<std::string> result;
    for(const auto& [positive,negative]:plot_differential_channels(p,id))
        for(const auto& key:{positive,negative})
            if(!key.empty()&&std::find(result.begin(),result.end(),key)==result.end())result.push_back(key);
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
