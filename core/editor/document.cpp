#include "core/editor/document.hpp"
#include <algorithm>
#include <set>
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
void Document::connect(Endpoint from,Endpoint to) {
    Wire wire{new_uuid(),std::move(from),std::move(to),{}};
    apply("Connect ports",[&](Project& p){validate_wire(p,wire);p.wires.push_back(wire);});
}
void Document::erase(const std::vector<std::string>& list) {
    const std::set<std::string> ids(list.begin(),list.end());
    apply("Delete objects",[&](Project& p) {
        std::erase_if(p.components,[&](const Component& c){return ids.count(c.id);});
        std::erase_if(p.nodes,[&](const Node& n){return ids.count(n.id);});
        std::erase_if(p.patterns,[&](const GatePattern& g){return ids.count(g.id);});
        std::erase_if(p.events,[&](const GateEvent& e){return ids.count(e.target);});
        std::erase_if(p.wires,[&](const Wire& w){return ids.count(w.id)||ids.count(w.from.object)||ids.count(w.to.object);});
    });
}
}
