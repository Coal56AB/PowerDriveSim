#include "core/editor/document.hpp"
#include "core/model/hierarchy.hpp"
#include "core/model/expression.hpp"
#include <algorithm>
#include <cctype>
#include <set>
#include <map>
#include <cmath>
#include <limits>
#include <optional>
#include <string_view>
namespace pds {
namespace {
std::pair<std::string, unsigned> name_stem(std::string name) {
    auto suffix = name.rfind(" (");
    if (suffix != std::string::npos && name.ends_with(")")) {
        const auto number = name.substr(suffix + 2, name.size() - suffix - 3);
        if (!number.empty() &&
            std::all_of(number.begin(), number.end(), [](unsigned char c) { return std::isdigit(c); }))
            name.erase(suffix);
    }
    size_t end = name.size();
    while (end > 0 && std::isdigit(static_cast<unsigned char>(name[end - 1])))
        --end;
    if (end < name.size()) {
        try {
            return {name.substr(0, end), static_cast<unsigned>(std::stoul(name.substr(end)))};
        } catch (...) {
        }
    }
    return {name, 1};
}
std::string next_name(std::string name, const std::set<std::string> &used) {
    if (name.empty() || !used.count(name))
        return name;
    auto [stem, number] = name_stem(std::move(name));
    for (unsigned suffix = std::max(2u, number + 1); suffix < 1000000; ++suffix) {
        auto candidate = stem + std::to_string(suffix);
        if (!used.count(candidate))
            return candidate;
    }
    return stem + std::to_string(used.size() + 1);
}
double point_distance(Point a, Point b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}
std::vector<Point> clean_bends(Point from, Point to, const std::vector<Point> &bends) {
    std::vector<Point> points{from};
    points.insert(points.end(), bends.begin(), bends.end());
    points.push_back(to);
    std::vector<Point> clean;
    for (auto point : points) {
        if (!clean.empty() && point_distance(clean.back(), point) < 1e-9)
            continue;
        while (clean.size() > 1) {
            const auto a = clean[clean.size() - 2], b = clean.back();
            const double ux = b.x - a.x, uy = b.y - a.y;
            const double vx = point.x - b.x, vy = point.y - b.y;
            if (std::abs(ux * vy - uy * vx) > 1e-9)
                break;
            clean.pop_back();
        }
        clean.push_back(point);
    }
    if (clean.size() <= 2)
        return {};
    return {clean.begin() + 1, clean.end() - 1};
}
Point endpoint_point(const Schematic &p, const Endpoint &endpoint) {
    auto find = [&](const auto &objects) -> std::optional<Point> {
        for (const auto &object : objects)
            if (object.id == endpoint.object)
                return Point{object.x, object.y};
        return {};
    };
    if (auto point = find(p.components))
        return *point;
    if (auto point = find(p.nodes))
        return *point;
    if (auto point = find(p.tags))
        return *point;
    if (auto point = find(p.patterns))
        return *point;
    if (auto point = find(p.plots))
        return *point;
    if (auto point = find(p.code_blocks))
        return *point;
    if (auto point = find(p.instances))
        return *point;
    return {};
}
bool generated_node_name(const std::string &name) {
    return name.empty();
}
bool endpoint_is_node(const Endpoint &endpoint, const std::string &node) {
    return endpoint.object == node && endpoint.port == "node";
}
bool channel_belongs_to(const std::set<std::string> &ids, const std::string &key) {
    if (ids.count(key))
        return true;
    constexpr std::string_view gate_prefix = "gate/";
    if (key.starts_with(gate_prefix) && ids.count(key.substr(gate_prefix.size())))
        return true;
    constexpr std::string_view difference_prefix = "diff/";
    if (key.starts_with(difference_prefix)) {
        const auto begin = difference_prefix.size();
        const auto end = key.find('/', begin);
        return ids.count(key.substr(begin, end - begin)) != 0;
    }
    return false;
}
bool target_belongs_to(const Project &root, const std::string &edited_definition,
                       const std::set<std::string> &ids, const ParameterTarget &target) {
    const Schematic *level=&root;
    std::string containing_definition;
    for(const auto &step:target.instances) {
        if(containing_definition==edited_definition&&ids.count(step))return true;
        const auto instance=std::find_if(level->instances.begin(),level->instances.end(),
            [&](const Instance &candidate){return candidate.id==step;});
        if(instance==level->instances.end())return false;
        containing_definition=instance->definition;
        level=&definition(root,containing_definition);
    }
    return containing_definition==edited_definition&&ids.count(target.object);
}
Endpoint other_endpoint(const Wire &wire, const std::string &node) {
    return endpoint_is_node(wire.from, node) ? wire.to : wire.from;
}
std::vector<Point> route_from_other_to_node(const Schematic &s, const Wire &wire, const std::string &node) {
    std::vector<Point> points;
    if (endpoint_is_node(wire.to, node)) {
        points.push_back(endpoint_point(s, wire.from));
        points.insert(points.end(), wire.bends.begin(), wire.bends.end());
        points.push_back(endpoint_point(s, wire.to));
    } else {
        points.push_back(endpoint_point(s, wire.to));
        points.insert(points.end(), wire.bends.rbegin(), wire.bends.rend());
        points.push_back(endpoint_point(s, wire.from));
    }
    return points;
}
bool merge_one_passthrough_node(Schematic &s) {
    for (const auto &node : s.nodes) {
        if (node.ground || !generated_node_name(node.name))
            continue;
        std::vector<size_t> incidents;
        for (size_t i = 0; i < s.wires.size(); ++i) {
            const auto &wire = s.wires[i];
            const bool from = endpoint_is_node(wire.from, node.id);
            const bool to = endpoint_is_node(wire.to, node.id);
            if (from && to)
                return false;
            if (from || to)
                incidents.push_back(i);
        }
        if (incidents.size() != 2)
            continue;
        const Wire first = s.wires[incidents[0]];
        const Wire second = s.wires[incidents[1]];
        const Endpoint from = other_endpoint(first, node.id);
        const Endpoint to = other_endpoint(second, node.id);
        if (from == to)
            continue;
        auto first_points = route_from_other_to_node(s, first, node.id);
        auto second_points = route_from_other_to_node(s, second, node.id);
        if (first_points.empty() || second_points.empty())
            continue;
        std::vector<Point> joined = first_points;
        joined.insert(joined.end(), second_points.rbegin() + 1, second_points.rend());
        if (joined.size() < 2)
            continue;
        Wire merged;
        merged.id = first.id;
        merged.from = from;
        merged.to = to;
        merged.color = first.color;
        merged.width = first.width;
        merged.line = first.line;
        std::vector<Point> bends;
        if (joined.size() > 2)
            bends.assign(joined.begin() + 1, joined.end() - 1);
        merged.bends = clean_bends(joined.front(), joined.back(), bends);
        const auto hi = std::max(incidents[0], incidents[1]);
        const auto lo = std::min(incidents[0], incidents[1]);
        s.wires.erase(s.wires.begin() + static_cast<std::ptrdiff_t>(hi));
        s.wires.erase(s.wires.begin() + static_cast<std::ptrdiff_t>(lo));
        s.wires.push_back(std::move(merged));
        std::erase_if(s.nodes, [&](const Node &candidate) { return candidate.id == node.id; });
        std::erase_if(s.labels, [&](const LabelLayout &label) { return label.object == node.id; });
        return true;
    }
    return false;
}
void normalize_wire_routes(Schematic &s) {
    while (merge_one_passthrough_node(s)) {
    }
    for (auto &wire : s.wires)
        wire.bends = clean_bends(endpoint_point(s, wire.from), endpoint_point(s, wire.to), wire.bends);
    while (merge_one_passthrough_node(s)) {
    }
}
void normalize_wire_routes(Project &p) {
    normalize_wire_routes(static_cast<Schematic &>(p));
    for (auto &definition : p.definitions)
        normalize_wire_routes(static_cast<Schematic &>(definition));
}
void preserve_views(Project& to,const Project& from) {
    auto preserve=[](Schematic& target,const Schematic& prior) {
        for(auto& plot:target.plots)for(const auto& old:prior.plots)if(old.id==plot.id){plot.begin=old.begin;plot.end=old.end;plot.cursor_a=old.cursor_a;plot.cursor_b=old.cursor_b;}
        for(const auto& options:prior.view_options)if(options.plot.empty()||std::any_of(target.plots.begin(),target.plots.end(),[&](const auto& plot){return plot.id==options.plot;})) {
            auto item=std::find_if(target.view_options.begin(),target.view_options.end(),[&](const auto& v){return v.plot==options.plot;});
            if(item==target.view_options.end())target.view_options.push_back(options);else *item=options;
        }
    };
    auto targets=flatten(to).project;
    targets.view_options=to.view_options;
    preserve(targets,from);
    to.view_options=std::move(targets.view_options);
    for(auto& plot:to.plots)for(const auto& old:from.plots)if(old.id==plot.id){plot.begin=old.begin;plot.end=old.end;plot.cursor_a=old.cursor_a;plot.cursor_b=old.cursor_b;}
    for(auto& d:to.definitions)for(const auto& old:from.definitions)if(d.id==old.id)preserve(d,old);
}
}
Document::Document(Project p):current_(make_wired(p)){normalize_wire_routes(current_);}
void Document::apply(const std::string& label,const std::function<void(Project&)>& change) {
    apply_with_root(label,change,[](Project&){});
}
void Document::apply_with_root(const std::string& label,const std::function<void(Project&)>& change,const std::function<void(Project&)>& finalize) {
    auto edited=project();change(edited);
    auto next=merge_view(edited);
    finalize(next);
    normalize_wire_routes(next);
    if(next == current_) return;
    // Connectivity is validated without solving an incomplete circuit.
    validate_hierarchy(next);
    if(!next.view_options.empty()) {
        const auto plots=flatten(next).project.plots;
        std::erase_if(next.view_options,[&](const auto& view){return !view.plot.empty()&&std::none_of(plots.begin(),plots.end(),[&](const auto& plot){return plot.id==view.plot;});});
    }
    (void)resolve_connections(next);
    undo_.push_back({label,current_,next,location_,location_});
    if(undo_.size()>100) undo_.erase(undo_.begin());
    current_=std::move(next); redo_.clear();rebuild_view();
}
void Document::undo() {
    if(undo_.empty()) return;
    auto change=std::move(undo_.back()); undo_.pop_back();
    auto views=current_; current_=change.before;location_=change.before_path;
    preserve_views(current_,views);
    current_.scope_begin=views.scope_begin;current_.scope_end=views.scope_end;current_.cursor_a=views.cursor_a;current_.cursor_b=views.cursor_b;
    rebuild_view();
    redo_.push_back(std::move(change));
}
void Document::redo() {
    if(redo_.empty()) return;
    auto change=std::move(redo_.back()); redo_.pop_back();
    auto views=current_; current_=change.after;location_=change.after_path;
    preserve_views(current_,views);
    current_.scope_begin=views.scope_begin;current_.scope_end=views.scope_end;current_.cursor_a=views.cursor_a;current_.cursor_b=views.cursor_b;
    rebuild_view();
    undo_.push_back(std::move(change));
}
void Document::set_view(const std::string& id,double begin,double end,double a,double b) {
    if(id.empty()){current_.scope_begin=begin;current_.scope_end=end;current_.cursor_a=a;current_.cursor_b=b;}
    else {
        auto flat=flatten(current_).project;
        if(std::none_of(flat.plots.begin(),flat.plots.end(),[&](const auto& plot){return plot.id==id;}))return;
        auto plot=std::find_if(current_.plots.begin(),current_.plots.end(),[&](const auto& plot){return plot.id==id;});
        if(plot!=current_.plots.end()){
            plot->begin=begin;plot->end=end;plot->cursor_a=a;plot->cursor_b=b;
            for(auto& view:current_.view_options)if(view.plot==id&&view.viewport){view.begin=begin;view.end=end;view.cursor_a=a;view.cursor_b=b;}
        }
        else {
            ViewOptions view;view.plot=id;
            for(const auto& prior:flat.view_options)if(prior.plot==id)view=prior;
            view.viewport=true;view.begin=begin;view.end=end;view.cursor_a=a;view.cursor_b=b;
            auto prior=std::find_if(current_.view_options.begin(),current_.view_options.end(),[&](const auto& v){return v.plot==id;});
            if(prior==current_.view_options.end())current_.view_options.push_back(view);else *prior=view;
        }
    }
    rebuild_view();
}
void Document::set_view_options(const ViewOptions& options) {
    const auto flat=flatten(current_).project;
    if(!options.plot.empty()&&std::none_of(flat.plots.begin(),flat.plots.end(),[&](const PlotBlock& p){return p.id==options.plot;}))return;
    auto updated=options;
    auto it=std::find_if(current_.view_options.begin(),current_.view_options.end(),[&](const ViewOptions& o){return o.plot==options.plot;});
    if(it!=current_.view_options.end()&&it->viewport) {
        updated.viewport=true;updated.begin=it->begin;updated.end=it->end;updated.cursor_a=it->cursor_a;updated.cursor_b=it->cursor_b;
    }
    if(it==current_.view_options.end())current_.view_options.push_back(updated);else *it=updated;
    rebuild_view();
}
bool same_simulation(const Project& a,const Project& b) {
    auto normalize=[](Project p){
        p.name.clear();
        p.experiments.clear();
        auto strip=[](auto& objects){for(auto& o:objects){o.name.clear();o.x=0;o.y=0;o.orientation={};}};
        auto strip_tags=[](auto& objects){for(auto& o:objects){o.x=0;o.y=0;o.orientation={};o.listed=true;o.connection_name.clear();o.scope_path.clear();}};
        auto schematic=[&](Schematic& s){
            strip(s.components);strip(s.nodes);strip_tags(s.tags);strip(s.patterns);strip(s.plots);strip(s.code_blocks);strip(s.instances);
            for(auto &block:s.code_blocks)block.icon.clear();
            s.object_icons.clear();
            for(auto& w:s.wires){w.bends.clear();w.color.clear();w.width=2;w.line=WireLine::automatic;}
            for(auto& g:s.patterns)g.pin_positions.clear();
            for(auto& g:s.plots){g.begin=0;g.end=-1;g.cursor_a=-1;g.cursor_b=-1;g.pin_positions.clear();}
            s.labels.clear();s.view_options.clear();
        };
        schematic(p);
        for(auto& d:p.definitions){d.name.clear();d.appearance={};schematic(d);for(auto& port:d.ports)port.name.clear();for(auto& param:d.parameters){if(!param.default_expression.empty())param.value=public_parameter_default_value(d,param);param.name.clear();param.unit.clear();param.group.clear();param.has_minimum=false;param.minimum=0;param.has_maximum=false;param.maximum=0;}}
        p.scope_begin=0;p.scope_end=-1;p.cursor_a=-1;p.cursor_b=-1;
        p.scope_enabled=false;p.scope_points.clear();p.scope_channels.clear();
        return p;
    };
    return normalize(a)==normalize(b);
}
void Document::set_experiments(const std::vector<Experiment>& experiments) {
    std::set<std::string> ids;
    for(const auto& e:experiments) {
        (void)experiment_size(e);
        if(!ids.insert(e.id).second)throw Diagnostic("invalid_uuid",e.id,"Duplicate experiment UUID");
    }
    apply_with_root("Edit experiments",[](Project&){},[&](Project& root){root.experiments=experiments;});
}
std::string Document::add_component(Kind kind,double x,double y) {
    auto id=new_uuid();
    apply("Add component",[&](Project& p) {
        double value=kind==Kind::resistor?1000:(kind==Kind::capacitor?1e-6:(kind==Kind::inductor?.01:(kind==Kind::voltage||kind==Kind::ideal_transformer?1:(kind==Kind::current?.001:0))));
        p.components.push_back({id,kind_name(kind)+std::to_string(p.components.size()+1),kind,"","",value,0,x,y,false});
    }); return id;
}
std::string Document::add_node(bool ground,double x,double y) {
    auto id=new_uuid(); apply("Add junction",[&](Project& p){p.nodes.push_back({id,ground?"GND":"N"+std::to_string(p.nodes.size()+1),ground,x,y});}); return id;
}
std::string Document::add_pattern(double x,double y) {
    auto id=new_uuid(); apply("Add gate pattern",[&](Project& p){p.patterns.push_back({id,"Gate"+std::to_string(p.patterns.size()+1),x,y,false});}); return id;
}
std::string Document::add_plot(double x,double y,const std::string& name,bool differential){
    auto id=new_uuid();apply("Add plot",[&](Project& p){
        PlotBlock plot{id,name+" "+std::to_string(p.plots.size()+1),x,y,2};
        plot.differential=differential;
        p.plots.push_back(std::move(plot));
    });return id;
}
void Document::connect(Endpoint from,Endpoint to) {
    Wire wire{new_uuid(),std::move(from),std::move(to),{}};
    apply("Connect ports",[&](Project& p){validate_wire(p,wire);p.wires.push_back(wire);});
}
void Document::connect_anchors(WireAnchor from,WireAnchor to,const std::vector<Point>& bends,const std::string& replace,bool replace_gate_driver) {
    apply("Connect wire",[&](Project& p){
        std::optional<Wire> replaced;
        if(!replace.empty()) {
            auto found=std::find_if(p.wires.begin(),p.wires.end(),[&](const Wire& w){return w.id==replace;});
            if(found!=p.wires.end())replaced=*found;
            std::erase_if(p.wires,[&](const Wire& w){return w.id==replace;});
        }
        auto attach=[&](const WireAnchor& anchor)->Endpoint {
            if(!anchor.endpoint.object.empty())return anchor.endpoint;
            if(!anchor.wire.empty()) {
                auto it=std::find_if(p.wires.begin(),p.wires.end(),[&](const Wire& w){return w.id==anchor.wire;});
                if(it==p.wires.end())throw Diagnostic("missing_wire",anchor.wire,"Wire no longer exists");
                auto type=port_type(p,it->from);
                if(type.domain!=Domain::electrical || port_type(p,it->to).domain!=Domain::electrical)
                    return type.direction==Direction::input?it->to:it->from;
                if(anchor.route.size()<2)throw Diagnostic("invalid_geometry",anchor.wire,"Missing wire route");
                if(point_distance(anchor.point,anchor.route.front())<1e-6)return it->from;
                if(point_distance(anchor.point,anchor.route.back())<1e-6)return it->to;
                size_t segment=1; double best=std::numeric_limits<double>::infinity();
                for(size_t i=1;i<anchor.route.size();++i){double d=point_distance(anchor.route[i-1],anchor.point)+point_distance(anchor.point,anchor.route[i])-point_distance(anchor.route[i-1],anchor.route[i]);if(d<best){best=d;segment=i;}}
                if(best>1e-6||!std::isfinite(anchor.point.x)||!std::isfinite(anchor.point.y))throw Diagnostic("invalid_geometry",anchor.wire,"Junction is outside wire route");
                auto id=new_uuid();Endpoint joint{id,"node"};
                p.nodes.push_back({id,"",false,anchor.point.x,anchor.point.y});
                Wire second{new_uuid(),joint,it->to,{}};
                second.color=it->color;second.width=it->width;second.line=it->line;
                second.bends.assign(anchor.route.begin()+segment,anchor.route.end()-1);
                it->to=joint;it->bends.assign(anchor.route.begin()+1,anchor.route.begin()+segment);
                it->bends=clean_bends(anchor.route.front(),anchor.point,it->bends);
                second.bends=clean_bends(anchor.point,anchor.route.back(),second.bends);
                p.wires.push_back(std::move(second));return joint;
            }
            auto id=new_uuid();p.nodes.push_back({id,"",false,anchor.point.x,anchor.point.y});return {id,"node"};
        };
        // A branch on the same wire is redundant and must not split it twice.
        if(!from.wire.empty()&&from.wire==to.wire)throw Diagnostic("duplicate_connection",from.wire,"Already connected");
        auto a=attach(from),b=attach(to);
        Wire wire{replace.empty()?new_uuid():replace,a,b,bends};
        if(replaced){wire.color=replaced->color;wire.width=replaced->width;wire.line=replaced->line;}
        wire.bends=clean_bends(endpoint_point(p,a),endpoint_point(p,b),wire.bends);
        validate_wire(p,wire);
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
    std::set<std::string> ids(list.begin(),list.end());Project result;result.id=new_uuid();result.name="Clipboard";result.profile=project().profile;result.wired=true;
    for(const auto& c:project().components)if(ids.count(c.id))result.components.push_back(c);
    for(const auto& n:project().nodes)if(ids.count(n.id))result.nodes.push_back(n);
    for(const auto& t:project().tags)if(ids.count(t.id))result.tags.push_back(t);
    for(const auto& g:project().patterns)if(ids.count(g.id))result.patterns.push_back(g);
    for(const auto& g:project().plots)if(ids.count(g.id))result.plots.push_back(g);
    for(const auto& block:project().code_blocks)if(ids.count(block.id))result.code_blocks.push_back(block);
    for(const auto& i:project().instances)if(ids.count(i.id))result.instances.push_back(i);
    if(!result.instances.empty())result.definitions=project().definitions;
    for(const auto& w:project().wires)if(ids.count(w.from.object)&&ids.count(w.to.object))result.wires.push_back(w);
    for(const auto& e:project().events)if(ids.count(e.target))result.events.push_back(e);
    for(const auto& expression:project().parameter_expressions)if(ids.count(expression.object))result.parameter_expressions.push_back(expression);
    for(const auto& appearance:project().object_icons)if(ids.count(appearance.object))result.object_icons.push_back(appearance);
    if(!result.parameter_expressions.empty())result.initialization_code=project().initialization_code;
    const auto fragment=flatten(result);
    const auto source=flatten(current_);
    std::map<std::string,std::string> channels;
    for(const auto& [local,origin]:fragment.origins) {
        auto path=location_;path.insert(path.end(),origin.instances.begin(),origin.instances.end());
        const auto global=expanded_uuid(path,origin.object);
        channels[global]=local;channels["gate/"+global]="gate/"+local;
    }
    const auto before=resolve_connections(current_).nets,after=resolve_connections(result).nets;
    for(const auto& [endpoint,net]:after) {
        const auto slash=endpoint.find('/');
        const auto global=expanded_uuid(location_,endpoint.substr(0,slash))+endpoint.substr(slash);
        if(auto found=before.find(global);found!=before.end())channels[found->second]=net;
    }
    for(auto options:source.project.view_options)if(channels.count(options.plot)) {
        remap_view_options(options,channels);result.view_options.push_back(std::move(options));
    }
    for(const auto& label:project().labels)if(ids.count(label.object))result.labels.push_back(label);
    return result;
}
std::vector<std::string> Document::paste(const Project& source,double dx,double dy){
    auto fragment=make_wired(source);std::vector<std::string> added;
    const auto original_fragment=flatten(fragment);
    const auto original_nets=resolve_connections(fragment).nets;
    apply("Paste objects",[&](Project& p){
        std::map<std::string,std::string> definitions;
        std::map<std::string,const Definition*> catalog;
        for(const auto& d:fragment.definitions)catalog.emplace(d.id,&d);
        std::set<std::string> remapping;
        std::function<std::string(const std::string&)> remap_definition=[&](const std::string& old)->std::string{
            if(auto found=definitions.find(old);found!=definitions.end())return found->second;
            if(!remapping.insert(old).second)throw Diagnostic("recursive_hierarchy",old,"Recursive subcircuit definition");
            auto source=catalog.find(old);
            if(source==catalog.end())throw Diagnostic("missing_definition",old,"Clipboard subcircuit definition is missing");
            auto copy=*source->second;
            bool dependency_changed=false;
            for(auto& child:copy.instances) {
                const auto nested=remap_definition(child.definition);
                dependency_changed|=nested!=child.definition;
                child.definition=nested;
            }
            const auto existing=std::find_if(p.definitions.begin(),p.definitions.end(),[&](const auto& d){return d.id==old;});
            const bool same=existing!=p.definitions.end()&&*existing==*source->second;
            const bool collision=existing!=p.definitions.end()&&*existing!=*source->second;
            const auto mapped=(collision||(same&&dependency_changed))?new_uuid():old;
            definitions[old]=mapped;
            if(!same||dependency_changed) {
                copy.id=mapped;
                p.definitions.push_back(std::move(copy));
            }
            remapping.erase(old);
            return mapped;
        };
        for(auto& i:fragment.instances)i.definition=remap_definition(i.definition);
        std::map<std::string,std::string> ids;std::set<std::string> names;
        auto remember=[&](const auto& objects){for(const auto& o:objects)names.insert(o.name);};remember(p.components);remember(p.nodes);remember(p.patterns);remember(p.plots);remember(p.code_blocks);remember(p.instances);
        auto copy=[&](const auto& from,auto& to){for(auto object:from){auto old=object.id;object.id=new_uuid();ids[old]=object.id;added.push_back(object.id);object.x+=dx;object.y+=dy;object.name=next_name(object.name,names);names.insert(object.name);to.push_back(std::move(object));}};
        auto copy_tags=[&](const auto& from,auto& to){for(auto object:from){auto old=object.id;object.id=new_uuid();ids[old]=object.id;added.push_back(object.id);object.x+=dx;object.y+=dy;to.push_back(std::move(object));}};
        copy(fragment.components,p.components);copy(fragment.nodes,p.nodes);copy_tags(fragment.tags,p.tags);copy(fragment.patterns,p.patterns);copy(fragment.plots,p.plots);copy(fragment.code_blocks,p.code_blocks);copy(fragment.instances,p.instances);
        for(auto appearance:fragment.object_icons)if(ids.count(appearance.object)){appearance.object=ids.at(appearance.object);p.object_icons.push_back(std::move(appearance));}
        for(auto wire:fragment.wires){if(!ids.count(wire.from.object)||!ids.count(wire.to.object))continue;wire.id=new_uuid();wire.from.object=ids.at(wire.from.object);wire.to.object=ids.at(wire.to.object);for(auto& point:wire.bends){point.x+=dx;point.y+=dy;}p.wires.push_back(std::move(wire));}
        for(auto event:fragment.events)if(ids.count(event.target)){event.target=ids.at(event.target);p.events.push_back(event);}
        const bool compatible_initialization=p.initialization_code.empty()||p.initialization_code==fragment.initialization_code;
        if(p.initialization_code.empty()&&!fragment.parameter_expressions.empty())p.initialization_code=fragment.initialization_code;
        if(compatible_initialization)for(auto expression:fragment.parameter_expressions)if(ids.count(expression.object)){
            expression.object=ids.at(expression.object);p.parameter_expressions.push_back(std::move(expression));
        }
        if(!fragment.view_options.empty()) {
            std::map<std::string,std::string> channels;
            for(const auto& [old,origin]:original_fragment.origins) {
                auto path=origin.instances;
                if(path.empty()) {
                    if(!ids.count(origin.object))continue;
                    channels[old]=ids.at(origin.object);
                } else {
                    if(!ids.count(path.front()))continue;
                    path.front()=ids.at(path.front());channels[old]=expanded_uuid(path,origin.object);
                }
                channels["gate/"+old]="gate/"+channels[old];
            }
            const auto after=resolve_connections(p).nets;
            for(const auto& [endpoint,net]:original_nets) {
                const auto slash=endpoint.find('/');
                const auto object=endpoint.substr(0,slash);
                if(!channels.count(object))continue;
                auto mapped=channels.find(object);
                if(mapped==channels.end())continue;
                if(auto found=after.find(mapped->second+endpoint.substr(slash));found!=after.end())channels[net]=found->second;
            }
            for(auto options:fragment.view_options)if(channels.count(options.plot)) {
                remap_view_options(options,channels);p.view_options.push_back(std::move(options));
            }
        }
        for(auto label:fragment.labels)if(ids.count(label.object)){label.object=ids.at(label.object);p.labels.push_back(label);}
    });return added;
}
void Document::transform(const std::vector<std::string>& list,int turns,bool mirror){
    std::set<std::string> ids(list.begin(),list.end());
    apply("Transform objects",[&](Project& p){
        double cx=0,cy=0;size_t count=0;
        auto center=[&](const auto& objects){for(const auto& o:objects)if(ids.count(o.id)){cx+=o.x;cy+=o.y;++count;}};
        center(p.components);center(p.nodes);center(p.tags);center(p.patterns);center(p.plots);center(p.code_blocks);center(p.instances);
        if(count){cx=std::round(cx/count/20)*20;cy=std::round(cy/count/20)*20;}
        auto point=[&](double& x,double& y){x-=cx;y-=cy;if(mirror)x=-x;else {int n=(turns%4+4)%4;while(n--){double old=x;x=-y;y=old;}}x+=cx;y+=cy;};
        auto change=[&](auto& objects){for(auto& object:objects)if(ids.count(object.id)){
            if(count>1)point(object.x,object.y);
            auto& o=object.orientation;if(mirror)o.mirrored=!o.mirrored;
            int amount=o.mirrored?-turns:turns;o.quarter_turns=static_cast<unsigned>((static_cast<int>(o.quarter_turns)+amount%4+4)%4);
        }};
        change(p.components);change(p.nodes);change(p.tags);change(p.patterns);change(p.plots);change(p.code_blocks);change(p.instances);
        if(count>1)for(auto& wire:p.wires)if(ids.count(wire.from.object)&&ids.count(wire.to.object))for(auto& b:wire.bends)point(b.x,b.y);
    });
}
void Document::arrange(const std::vector<std::string>& list,const std::string& mode){
    std::set<std::string> ids(list.begin(),list.end());
    apply("Arrange objects",[&](Project& p){std::vector<std::pair<double*,double*>> points;auto add=[&](auto& objects){for(auto& o:objects)if(ids.count(o.id))points.push_back({&o.x,&o.y});};add(p.components);add(p.nodes);add(p.tags);add(p.patterns);add(p.plots);add(p.code_blocks);add(p.instances);if(points.size()<2)return;
        bool horizontal=mode=="left"||mode=="right"||mode=="horizontal";
        auto coordinate=[&](auto point)->double&{return horizontal?*point.first:*point.second;};
        std::sort(points.begin(),points.end(),[&](auto a,auto b){return coordinate(a)<coordinate(b);});double low=coordinate(points.front()),high=coordinate(points.back());
        for(size_t i=0;i<points.size();++i){double value=low;if(mode=="right"||mode=="bottom")value=high;if(mode=="horizontal"||mode=="vertical")value=low+(high-low)*i/(points.size()-1);coordinate(points[i])=std::round(value/20)*20;}
    });
}
void Document::erase(const std::vector<std::string>& list,
                     const std::vector<std::string>& extension_records) {
    const std::set<std::string> ids(list.begin(),list.end());
    const std::set<std::string> records(extension_records.begin(),extension_records.end());
    const auto edited_definition=location_.empty()?std::string{}:current_definition();
    if(!location_.empty()) {
        const auto &active=definition(current_,current_definition());
        for(const auto &port:active.ports)if(ids.count(port.terminal.object))
            throw Diagnostic("invalid_parameter_binding",port.terminal.object,
                             "Remove or redirect the public port before deleting its internal object");
        for(const auto &parameter:active.parameters)if(parameter.object!="*"&&ids.count(parameter.object))
            throw Diagnostic("invalid_parameter_binding",parameter.object,
                             "Remove or redirect the public parameter before deleting its internal object");
    }
    auto root_ids=ids;
    const bool has_dependencies=!current_.scope_points.empty()||!current_.scope_channels.empty()||
        std::any_of(current_.experiments.begin(),current_.experiments.end(),[](const Experiment &experiment){
            if(!experiment.channels.empty()||!experiment.axes.empty())return true;
            return std::any_of(experiment.scenarios.begin(),experiment.scenarios.end(),
                [](const Scenario &scenario){return !scenario.overrides.empty();});
        });
    if(has_dependencies) {
        const auto expanded=flatten(current_);
        for(const auto &[identity,origin]:expanded.origins) {
            const Schematic *level=&current_;
            std::string containing_definition;
            bool affected=false,valid=true;
            for(const auto &step:origin.instances) {
                if(containing_definition==edited_definition&&ids.count(step))affected=true;
                const auto instance=std::find_if(level->instances.begin(),level->instances.end(),
                    [&](const Instance &candidate){return candidate.id==step;});
                if(instance==level->instances.end()){valid=false;break;}
                containing_definition=instance->definition;
                level=&definition(current_,containing_definition);
            }
            if(valid&&containing_definition==edited_definition&&ids.count(origin.object))affected=true;
            if(valid&&affected)root_ids.insert(identity);
        }
    }
    apply_with_root("Delete objects",[&](Project& p) {
        std::erase_if(p.extensions,[&](const std::string& record){return records.count(record)>0;});
        const bool removes_object=
            std::any_of(p.components.begin(),p.components.end(),[&](const Component& c){return ids.count(c.id);})||
            std::any_of(p.tags.begin(),p.tags.end(),[&](const ConnectionTag& t){return ids.count(t.id);})||
            std::any_of(p.plots.begin(),p.plots.end(),[&](const PlotBlock& g){return ids.count(g.id);})||
            std::any_of(p.code_blocks.begin(),p.code_blocks.end(),[&](const CodeBlock& block){return ids.count(block.id);})||
            std::any_of(p.patterns.begin(),p.patterns.end(),[&](const GatePattern& g){return ids.count(g.id);})||
            std::any_of(p.instances.begin(),p.instances.end(),[&](const Instance& i){return ids.count(i.id);});
        std::erase_if(p.labels,[&](const LabelLayout& l){return ids.count(l.object);});
        std::erase_if(p.view_options,[&](const ViewOptions& o){return ids.count(o.plot);});
        std::erase_if(p.object_icons,[&](const ObjectIcon& appearance){return ids.count(appearance.object);});
        std::erase_if(p.components,[&](const Component& c){return ids.count(c.id);});
        std::erase_if(p.nodes,[&](const Node& n){return ids.count(n.id);});
        std::erase_if(p.tags,[&](const ConnectionTag& t){return ids.count(t.id);});
        std::erase_if(p.plots,[&](const PlotBlock& g){return ids.count(g.id);});
        std::erase_if(p.code_blocks,[&](const CodeBlock& block){return ids.count(block.id);});
        std::erase_if(p.patterns,[&](const GatePattern& g){return ids.count(g.id);});
        std::erase_if(p.instances,[&](const Instance& i){return ids.count(i.id);});
        std::erase_if(p.events,[&](const GateEvent& e){return ids.count(e.target);});
        std::erase_if(p.parameter_expressions,[&](const ParameterExpression& expression){return ids.count(expression.object);});
        std::erase_if(p.wires,[&](const Wire& w){return ids.count(w.id)||ids.count(w.from.object)||ids.count(w.to.object);});
        std::erase_if(p.scope_points,[&](const std::string &key){return channel_belongs_to(ids,key);});
        std::erase_if(p.scope_channels,[&](const std::string &key){return channel_belongs_to(ids,key);});
        // A junction without a single incident conductor has no electrical or
        // visual meaning. Keeping it produced a small unexplained square after
        // deleting the object or wire that owned the last connection.
        if(removes_object) {
            std::erase_if(p.nodes,[&](const Node& n){
                if(n.ground)return false;
                return std::none_of(p.wires.begin(),p.wires.end(),[&](const Wire& w){
                    return w.from.object==n.id||w.to.object==n.id;
                });
            });
            std::erase_if(p.labels,[&](const LabelLayout& l){
                return std::none_of(p.nodes.begin(),p.nodes.end(),[&](const Node& n){return n.id==l.object;})&&
                       std::none_of(p.components.begin(),p.components.end(),[&](const Component& c){return c.id==l.object;})&&
                       std::none_of(p.tags.begin(),p.tags.end(),[&](const ConnectionTag& t){return t.id==l.object;})&&
                       std::none_of(p.plots.begin(),p.plots.end(),[&](const PlotBlock& g){return g.id==l.object;})&&
                       std::none_of(p.code_blocks.begin(),p.code_blocks.end(),[&](const CodeBlock& block){return block.id==l.object;})&&
                       std::none_of(p.patterns.begin(),p.patterns.end(),[&](const GatePattern& g){return g.id==l.object;})&&
                       std::none_of(p.instances.begin(),p.instances.end(),[&](const Instance& i){return i.id==l.object;});
            });
        }
    },[&](Project &root){
        const auto remove=[&](const std::string &key){return channel_belongs_to(root_ids,key);};
        std::erase_if(root.scope_points,remove);
        std::erase_if(root.scope_channels,remove);
        for(auto &experiment:root.experiments) {
            std::erase_if(experiment.channels,remove);
            std::erase_if(experiment.axes,[&](const SweepAxis &axis){
                return target_belongs_to(current_,edited_definition,ids,axis.target);
            });
            for(auto &scenario:experiment.scenarios)
                std::erase_if(scenario.overrides,[&](const ParameterOverride &override){
                    return target_belongs_to(current_,edited_definition,ids,override.target);
                });
        }
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
        for(auto& key:p.scope_points)if(key==old_net)key=new_net;
        for(auto& key:p.scope_channels)if(key==old_net)key=new_net;
    });
}
}
