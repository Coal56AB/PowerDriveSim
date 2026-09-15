#include "core/editor/document.hpp"
#include <algorithm>
#include <set>
#include <map>
#include <cmath>
namespace pds {
Document::Document(Project p):current_(make_wired(p)){}
void Document::apply(const std::string& label,const std::function<void(Project&)>& change) {
    auto next=current_; change(next);
    // Connectivity is validated without solving an incomplete circuit.
    (void)resolve_connections(next);
    undo_.push_back({label,current_,next});
    if(undo_.size()>100) undo_.erase(undo_.begin());
    current_=std::move(next); redo_.clear();
}
void Document::undo() {
    if(undo_.empty()) return;
    auto change=std::move(undo_.back()); undo_.pop_back();
    current_=change.before; redo_.push_back(std::move(change));
}
void Document::redo() {
    if(redo_.empty()) return;
    auto change=std::move(redo_.back()); redo_.pop_back();
    current_=change.after; undo_.push_back(std::move(change));
}
std::string Document::add_component(Kind kind,double x,double y) {
    auto id=new_uuid();
    apply("Add component",[&](Project& p) {
        double value=kind==Kind::resistor?1000:(kind==Kind::capacitor?1e-6:(kind==Kind::inductor?.01:(kind==Kind::voltage?1:(kind==Kind::current?.001:0))));
        p.components.push_back({id,kind_name(kind)+std::to_string(p.components.size()+1),kind,"","",value,0,x,y,false});
    }); return id;
}
std::string Document::add_node(bool ground,double x,double y) {
    auto id=new_uuid(); apply("Add junction",[&](Project& p){p.nodes.push_back({id,ground?"GND":"N"+std::to_string(p.nodes.size()+1),ground,x,y});}); return id;
}
std::string Document::add_pattern(double x,double y) {
    auto id=new_uuid(); apply("Add gate pattern",[&](Project& p){p.patterns.push_back({id,"Gate"+std::to_string(p.patterns.size()+1),x,y,false});}); return id;
}
std::string Document::add_plot(double x,double y,const std::string& name){
    auto id=new_uuid();apply("Add plot",[&](Project& p){p.plots.push_back({id,name+" "+std::to_string(p.plots.size()+1),x,y,2});});return id;
}
void Document::connect(Endpoint from,Endpoint to) {
    Wire wire{new_uuid(),std::move(from),std::move(to),{}};
    apply("Connect ports",[&](Project& p){validate_wire(p,wire);p.wires.push_back(wire);});
}
Project Document::copy(const std::vector<std::string>& list) const {
    std::set<std::string> ids(list.begin(),list.end());Project result;result.id=new_uuid();result.name="Clipboard";result.profile=current_.profile;result.wired=true;
    for(const auto& c:current_.components)if(ids.count(c.id))result.components.push_back(c);
    for(const auto& n:current_.nodes)if(ids.count(n.id))result.nodes.push_back(n);
    for(const auto& g:current_.patterns)if(ids.count(g.id))result.patterns.push_back(g);
    for(const auto& g:current_.plots)if(ids.count(g.id))result.plots.push_back(g);
    for(const auto& w:current_.wires)if(ids.count(w.from.object)&&ids.count(w.to.object))result.wires.push_back(w);
    for(const auto& e:current_.events)if(ids.count(e.target))result.events.push_back(e);
    return result;
}
std::vector<std::string> Document::paste(const Project& source,double dx,double dy){
    auto fragment=make_wired(source);std::vector<std::string> added;
    apply("Paste objects",[&](Project& p){
        std::map<std::string,std::string> ids;std::set<std::string> names;
        auto remember=[&](const auto& objects){for(const auto& o:objects)names.insert(o.name);};remember(p.components);remember(p.nodes);remember(p.patterns);remember(p.plots);
        auto copy=[&](const auto& from,auto& to){for(auto object:from){auto old=object.id;object.id=new_uuid();ids[old]=object.id;added.push_back(object.id);object.x+=dx;object.y+=dy;auto base=object.name;unsigned suffix=2;while(names.count(object.name))object.name=base+" ("+std::to_string(suffix++)+")";names.insert(object.name);to.push_back(std::move(object));}};
        copy(fragment.components,p.components);copy(fragment.nodes,p.nodes);copy(fragment.patterns,p.patterns);copy(fragment.plots,p.plots);
        for(auto wire:fragment.wires){if(!ids.count(wire.from.object)||!ids.count(wire.to.object))continue;wire.id=new_uuid();wire.from.object=ids.at(wire.from.object);wire.to.object=ids.at(wire.to.object);for(auto& point:wire.bends){point.x+=dx;point.y+=dy;}p.wires.push_back(std::move(wire));}
        for(auto event:fragment.events)if(ids.count(event.target)){event.target=ids.at(event.target);p.events.push_back(event);}
    });return added;
}
void Document::transform(const std::vector<std::string>& list,int turns,bool mirror){
    std::set<std::string> ids(list.begin(),list.end());
    apply("Transform objects",[&](Project& p){auto change=[&](auto& objects){for(auto& object:objects)if(ids.count(object.id)){auto& o=object.orientation;if(mirror)o.mirrored=!o.mirrored;int amount=o.mirrored?-turns:turns;o.quarter_turns=static_cast<unsigned>((static_cast<int>(o.quarter_turns)+amount%4+4)%4);}};change(p.components);change(p.nodes);change(p.patterns);change(p.plots);});
}
void Document::arrange(const std::vector<std::string>& list,const std::string& mode){
    std::set<std::string> ids(list.begin(),list.end());
    apply("Arrange objects",[&](Project& p){std::vector<std::pair<double*,double*>> points;auto add=[&](auto& objects){for(auto& o:objects)if(ids.count(o.id))points.push_back({&o.x,&o.y});};add(p.components);add(p.nodes);add(p.patterns);add(p.plots);if(points.size()<2)return;
        bool horizontal=mode=="left"||mode=="right"||mode=="horizontal";
        auto coordinate=[&](auto point)->double&{return horizontal?*point.first:*point.second;};
        std::sort(points.begin(),points.end(),[&](auto a,auto b){return coordinate(a)<coordinate(b);});double low=coordinate(points.front()),high=coordinate(points.back());
        for(size_t i=0;i<points.size();++i){double value=low;if(mode=="right"||mode=="bottom")value=high;if(mode=="horizontal"||mode=="vertical")value=low+(high-low)*i/(points.size()-1);coordinate(points[i])=std::round(value/20)*20;}
    });
}
void Document::erase(const std::vector<std::string>& list) {
    const std::set<std::string> ids(list.begin(),list.end());
    apply("Delete objects",[&](Project& p) {
        std::erase_if(p.components,[&](const Component& c){return ids.count(c.id);});
        std::erase_if(p.nodes,[&](const Node& n){return ids.count(n.id);});
        std::erase_if(p.plots,[&](const PlotBlock& g){return ids.count(g.id);});
        std::erase_if(p.patterns,[&](const GatePattern& g){return ids.count(g.id);});
        std::erase_if(p.events,[&](const GateEvent& e){return ids.count(e.target);});
        std::erase_if(p.wires,[&](const Wire& w){return ids.count(w.id)||ids.count(w.from.object)||ids.count(w.to.object);});
    });
}
}
