#include "core/editor/document.hpp"
#include "core/model/hierarchy.hpp"
#include <algorithm>
#include <set>
#include <map>
#include <cmath>
#include <limits>
namespace pds {
Document::Document(Project p):current_(make_wired(p)){}
void Document::apply(const std::string& label,const std::function<void(Project&)>& change) {
    auto next=current_; change(next);
    if(next == current_) return;
    // Connectivity is validated without solving an incomplete circuit.
    validate_hierarchy(next);
    (void)resolve_connections(next);
    undo_.push_back({label,current_,next});
    if(undo_.size()>100) undo_.erase(undo_.begin());
    current_=std::move(next); redo_.clear();
}
void Document::undo() {
    if(undo_.empty()) return;
    auto change=std::move(undo_.back()); undo_.pop_back();
    auto views=current_; current_=change.before;
    for(const auto& options:views.view_options)set_view_options(options);
    set_view("",views.scope_begin,views.scope_end,views.cursor_a,views.cursor_b);
    for(const auto& plot:views.plots) set_view(plot.id,plot.begin,plot.end,plot.cursor_a,plot.cursor_b);
    redo_.push_back(std::move(change));
}
void Document::redo() {
    if(redo_.empty()) return;
    auto change=std::move(redo_.back()); redo_.pop_back();
    auto views=current_; current_=change.after;
    for(const auto& options:views.view_options)set_view_options(options);
    set_view("",views.scope_begin,views.scope_end,views.cursor_a,views.cursor_b);
    for(const auto& plot:views.plots) set_view(plot.id,plot.begin,plot.end,plot.cursor_a,plot.cursor_b);
    undo_.push_back(std::move(change));
}
void Document::set_view(const std::string& id,double begin,double end,double a,double b) {
    if(id.empty()){current_.scope_begin=begin;current_.scope_end=end;current_.cursor_a=a;current_.cursor_b=b;}
    else for(auto& plot:current_.plots) if(plot.id==id){plot.begin=begin;plot.end=end;plot.cursor_a=a;plot.cursor_b=b;break;}
}
void Document::set_view_options(const ViewOptions& options) {
    if(!options.plot.empty()&&std::none_of(current_.plots.begin(),current_.plots.end(),[&](const PlotBlock& p){return p.id==options.plot;}))return;
    auto it=std::find_if(current_.view_options.begin(),current_.view_options.end(),[&](const ViewOptions& o){return o.plot==options.plot;});
    if(it==current_.view_options.end())current_.view_options.push_back(options);else *it=options;
}
bool same_simulation(const Project& a,const Project& b) {
    auto normalize=[](Project p){
        p.name.clear();
        auto strip=[](auto& objects){for(auto& o:objects){o.name.clear();o.x=0;o.y=0;o.orientation={};}};
        auto schematic=[&](Schematic& s){
            strip(s.components);strip(s.nodes);strip(s.patterns);strip(s.plots);strip(s.instances);
            for(auto& w:s.wires)w.bends.clear();
            for(auto& g:s.plots){g.begin=0;g.end=-1;g.cursor_a=-1;g.cursor_b=-1;}
            s.labels.clear();s.view_options.clear();
        };
        schematic(p);
        for(auto& d:p.definitions){d.name.clear();schematic(d);for(auto& port:d.ports)port.name.clear();for(auto& param:d.parameters){param.name.clear();param.unit.clear();}}
        p.scope_begin=0;p.scope_end=-1;p.cursor_a=-1;p.cursor_b=-1;
        p.scope_enabled=false;p.scope_channels.clear();
        return p;
    };
    return normalize(a)==normalize(b);
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
void Document::connect_anchors(WireAnchor from,WireAnchor to,const std::vector<Point>& bends,const std::string& replace,bool replace_gate_driver) {
    apply("Connect wire",[&](Project& p){
        if(!replace.empty())std::erase_if(p.wires,[&](const Wire& w){return w.id==replace;});
        auto attach=[&](const WireAnchor& anchor)->Endpoint {
            if(!anchor.endpoint.object.empty())return anchor.endpoint;
            if(!anchor.wire.empty()) {
                auto it=std::find_if(p.wires.begin(),p.wires.end(),[&](const Wire& w){return w.id==anchor.wire;});
                if(it==p.wires.end())throw Diagnostic("missing_wire",anchor.wire,"Wire no longer exists");
                auto type=port_type(p,it->from);
                if(type.domain!=Domain::electrical || port_type(p,it->to).domain!=Domain::electrical)
                    return type.direction==Direction::input?it->to:it->from;
                if(anchor.route.size()<2)throw Diagnostic("invalid_geometry",anchor.wire,"Missing wire route");
                auto distance=[](Point a,Point b){return std::hypot(a.x-b.x,a.y-b.y);};
                if(distance(anchor.point,anchor.route.front())<1e-6)return it->from;
                if(distance(anchor.point,anchor.route.back())<1e-6)return it->to;
                size_t segment=1; double best=std::numeric_limits<double>::infinity();
                for(size_t i=1;i<anchor.route.size();++i){double d=distance(anchor.route[i-1],anchor.point)+distance(anchor.point,anchor.route[i])-distance(anchor.route[i-1],anchor.route[i]);if(d<best){best=d;segment=i;}}
                if(best>1e-6||!std::isfinite(anchor.point.x)||!std::isfinite(anchor.point.y))throw Diagnostic("invalid_geometry",anchor.wire,"Junction is outside wire route");
                auto id=new_uuid();Endpoint joint{id,"node"};
                p.nodes.push_back({id,"",false,anchor.point.x,anchor.point.y});
                Wire second{new_uuid(),joint,it->to,{}};
                second.bends.assign(anchor.route.begin()+segment,anchor.route.end()-1);
                it->to=joint;it->bends.assign(anchor.route.begin()+1,anchor.route.begin()+segment);
                p.wires.push_back(std::move(second));return joint;
            }
            auto id=new_uuid();p.nodes.push_back({id,"",false,anchor.point.x,anchor.point.y});return {id,"node"};
        };
        // A branch on the same wire is redundant and must not split it twice.
        if(!from.wire.empty()&&from.wire==to.wire)throw Diagnostic("duplicate_connection",from.wire,"Already connected");
        auto a=attach(from),b=attach(to);
        Wire wire{replace.empty()?new_uuid():replace,a,b,bends};validate_wire(p,wire);
        if(replace_gate_driver) {
            auto input=port_type(p,a).direction==Direction::input?a:b;
            if(port_type(p,input).domain==Domain::gate && port_type(p,input).direction==Direction::input) {
                std::erase_if(p.events,[&](const GateEvent& e){return e.target==input.object;});
                std::erase_if(p.wires,[&](const Wire& w){return w.from==input||w.to==input;});
            }
        }
        p.wires.push_back(std::move(wire));
    });
}
Project Document::copy(const std::vector<std::string>& list) const {
    std::set<std::string> ids(list.begin(),list.end());Project result;result.id=new_uuid();result.name="Clipboard";result.profile=current_.profile;result.wired=true;
    for(const auto& c:current_.components)if(ids.count(c.id))result.components.push_back(c);
    for(const auto& n:current_.nodes)if(ids.count(n.id))result.nodes.push_back(n);
    for(const auto& g:current_.patterns)if(ids.count(g.id))result.patterns.push_back(g);
    for(const auto& g:current_.plots)if(ids.count(g.id))result.plots.push_back(g);
    for(const auto& i:current_.instances)if(ids.count(i.id))result.instances.push_back(i);
    if(!result.instances.empty())result.definitions=current_.definitions;
    for(const auto& w:current_.wires)if(ids.count(w.from.object)&&ids.count(w.to.object))result.wires.push_back(w);
    for(const auto& e:current_.events)if(ids.count(e.target))result.events.push_back(e);
    for(const auto& options:current_.view_options)if(ids.count(options.plot))result.view_options.push_back(options);
    for(const auto& label:current_.labels)if(ids.count(label.object))result.labels.push_back(label);
    return result;
}
std::vector<std::string> Document::paste(const Project& source,double dx,double dy){
    auto fragment=make_wired(source);std::vector<std::string> added;
    apply("Paste objects",[&](Project& p){
        std::map<std::string,std::string> definitions;
        const bool conflict=std::any_of(fragment.definitions.begin(),fragment.definitions.end(),[&](const auto& d){return std::any_of(p.definitions.begin(),p.definitions.end(),[&](const auto& existing){return existing.id==d.id&&existing!=d;});});
        for(const auto& d:fragment.definitions) {
            definitions[d.id]=conflict?new_uuid():d.id;
        }
        for(auto d:fragment.definitions) {
            d.id=definitions.at(d.id);
            if(std::any_of(p.definitions.begin(),p.definitions.end(),[&](const auto& v){return v.id==d.id;}))continue;
            for(auto& i:d.instances)i.definition=definitions.at(i.definition);
            p.definitions.push_back(std::move(d));
        }
        for(auto& i:fragment.instances)i.definition=definitions.at(i.definition);
        std::map<std::string,std::string> ids;std::set<std::string> names;
        auto remember=[&](const auto& objects){for(const auto& o:objects)names.insert(o.name);};remember(p.components);remember(p.nodes);remember(p.patterns);remember(p.plots);
        auto copy=[&](const auto& from,auto& to){for(auto object:from){auto old=object.id;object.id=new_uuid();ids[old]=object.id;added.push_back(object.id);object.x+=dx;object.y+=dy;auto base=object.name;unsigned suffix=2;while(names.count(object.name))object.name=base+" ("+std::to_string(suffix++)+")";names.insert(object.name);to.push_back(std::move(object));}};
        copy(fragment.components,p.components);copy(fragment.nodes,p.nodes);copy(fragment.patterns,p.patterns);copy(fragment.plots,p.plots);copy(fragment.instances,p.instances);
        for(auto wire:fragment.wires){if(!ids.count(wire.from.object)||!ids.count(wire.to.object))continue;wire.id=new_uuid();wire.from.object=ids.at(wire.from.object);wire.to.object=ids.at(wire.to.object);for(auto& point:wire.bends){point.x+=dx;point.y+=dy;}p.wires.push_back(std::move(wire));}
        for(auto event:fragment.events)if(ids.count(event.target)){event.target=ids.at(event.target);p.events.push_back(event);}
        for(auto options:fragment.view_options)if(ids.count(options.plot)){options.plot=ids.at(options.plot);p.view_options.push_back(options);}
        for(auto label:fragment.labels)if(ids.count(label.object)){label.object=ids.at(label.object);p.labels.push_back(label);}
    });return added;
}
void Document::transform(const std::vector<std::string>& list,int turns,bool mirror){
    std::set<std::string> ids(list.begin(),list.end());
    apply("Transform objects",[&](Project& p){
        double cx=0,cy=0;size_t count=0;
        auto center=[&](const auto& objects){for(const auto& o:objects)if(ids.count(o.id)){cx+=o.x;cy+=o.y;++count;}};
        center(p.components);center(p.nodes);center(p.patterns);center(p.plots);center(p.instances);
        if(count){cx=std::round(cx/count/20)*20;cy=std::round(cy/count/20)*20;}
        auto point=[&](double& x,double& y){x-=cx;y-=cy;if(mirror)x=-x;else {int n=(turns%4+4)%4;while(n--){double old=x;x=-y;y=old;}}x+=cx;y+=cy;};
        auto change=[&](auto& objects){for(auto& object:objects)if(ids.count(object.id)){
            if(count>1)point(object.x,object.y);
            auto& o=object.orientation;if(mirror)o.mirrored=!o.mirrored;
            int amount=o.mirrored?-turns:turns;o.quarter_turns=static_cast<unsigned>((static_cast<int>(o.quarter_turns)+amount%4+4)%4);
        }};
        change(p.components);change(p.nodes);change(p.patterns);change(p.plots);change(p.instances);
        if(count>1)for(auto& wire:p.wires)if(ids.count(wire.from.object)&&ids.count(wire.to.object))for(auto& b:wire.bends)point(b.x,b.y);
    });
}
void Document::arrange(const std::vector<std::string>& list,const std::string& mode){
    std::set<std::string> ids(list.begin(),list.end());
    apply("Arrange objects",[&](Project& p){std::vector<std::pair<double*,double*>> points;auto add=[&](auto& objects){for(auto& o:objects)if(ids.count(o.id))points.push_back({&o.x,&o.y});};add(p.components);add(p.nodes);add(p.patterns);add(p.plots);add(p.instances);if(points.size()<2)return;
        bool horizontal=mode=="left"||mode=="right"||mode=="horizontal";
        auto coordinate=[&](auto point)->double&{return horizontal?*point.first:*point.second;};
        std::sort(points.begin(),points.end(),[&](auto a,auto b){return coordinate(a)<coordinate(b);});double low=coordinate(points.front()),high=coordinate(points.back());
        for(size_t i=0;i<points.size();++i){double value=low;if(mode=="right"||mode=="bottom")value=high;if(mode=="horizontal"||mode=="vertical")value=low+(high-low)*i/(points.size()-1);coordinate(points[i])=std::round(value/20)*20;}
    });
}
void Document::erase(const std::vector<std::string>& list) {
    const std::set<std::string> ids(list.begin(),list.end());
    apply("Delete objects",[&](Project& p) {
        std::erase_if(p.labels,[&](const LabelLayout& l){return ids.count(l.object);});
        std::erase_if(p.view_options,[&](const ViewOptions& o){return ids.count(o.plot);});
        std::erase_if(p.components,[&](const Component& c){return ids.count(c.id);});
        std::erase_if(p.nodes,[&](const Node& n){return ids.count(n.id);});
        std::erase_if(p.plots,[&](const PlotBlock& g){return ids.count(g.id);});
        std::erase_if(p.patterns,[&](const GatePattern& g){return ids.count(g.id);});
        std::erase_if(p.instances,[&](const Instance& i){return ids.count(i.id);});
        std::erase_if(p.events,[&](const GateEvent& e){return ids.count(e.target);});
        std::erase_if(p.wires,[&](const Wire& w){return ids.count(w.id)||ids.count(w.from.object)||ids.count(w.to.object);});
    });
}
void Document::remove_junction(const std::string& id,std::vector<Point> a,std::vector<Point> b) {
    apply("Remove pass-through junction",[&](Project& p){
        auto node=std::find_if(p.nodes.begin(),p.nodes.end(),[&](const Node& n){return n.id==id&&!n.ground;});
        std::vector<Wire> wires;for(const auto& w:p.wires)if(w.from.object==id||w.to.object==id)wires.push_back(w);
        if(node==p.nodes.end()||wires.size()!=2||a.size()<2||b.size()<2)throw Diagnostic("invalid_junction",id,"Only a two-wire junction can be removed without disconnecting");
        auto first=wires[0],second=wires[1];
        if(first.from.object==id){std::swap(first.from,first.to);std::reverse(a.begin(),a.end());}
        if(second.to.object==id){std::swap(second.from,second.to);std::reverse(b.begin(),b.end());}
        const auto old_net=resolve_connections(p).nets.at(endpoint_key(first.from));
        first.to=second.to;a.insert(a.end(),b.begin()+1,b.end());first.bends.assign(a.begin()+1,a.end()-1);
        std::erase_if(p.wires,[&](const Wire& w){return w.from.object==id||w.to.object==id;});
        if(first.from!=first.to&&!std::any_of(p.wires.begin(),p.wires.end(),[&](const Wire& w){return (w.from==first.from&&w.to==first.to)||(w.to==first.from&&w.from==first.to);}))p.wires.push_back(first);
        std::erase_if(p.nodes,[&](const Node& n){return n.id==id;});
        std::erase_if(p.labels,[&](const LabelLayout& l){return l.object==id;});
        const auto new_net=resolve_connections(p).nets.at(endpoint_key(first.from));
        for(auto& key:p.scope_channels)if(key==old_net)key=new_net;
    });
}
}
