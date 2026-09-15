#include "formats/project/project.hpp"
#include <iomanip>
#include <map>
#include <istream>
#include <ostream>
#include <sstream>
namespace pds {
Project read_project(std::istream& in) {
    Project p;
    std::string line, tag;
    if(!std::getline(in,line)) throw Diagnostic("parse_error","","Empty project");
    if(line.compare(0,3,"\xef\xbb\xbf")==0) line.erase(0,3);
    if(!line.empty() && line.back()=='\r') line.pop_back();
    std::istringstream header(line);
    if(!(header >> tag >> p.schema)) throw Diagnostic("parse_error","","Malformed header");
    header >> std::ws;
    if(!header.eof() || tag!="PowerDriveSim" || (p.schema<1 || p.schema>6))
        throw Diagnostic("schema_version","","Expected PowerDriveSim schema 1..6");
    bool identity=false, profile=false, nonlinear=false, wiring=false, recording=false;
    std::map<std::string,Orientation> orientations;
    size_t number=1;
    while(std::getline(in,line)) {
        ++number;
        if(!line.empty() && line.back()=='\r') line.pop_back();
        if(line.empty() || line[0]=='#') continue;
        std::istringstream row(line);
        row >> tag;
        if(tag.rfind("x-",0)==0) { p.extensions.push_back(line); continue; }
        if(tag=="project" && !identity) {
            row >> std::quoted(p.id) >> std::quoted(p.name); identity=true;
        } else if(tag=="profile" && !profile) {
            row >> p.profile.stop >> p.profile.step;
            if(p.schema>=2) {
                std::string method;
                if(!(row >> method)) throw Diagnostic("parse_error",std::to_string(number),"Missing integration method");
                p.profile.method=parse_method(method);
            }
            profile=true;
        } else if(tag=="nonlinear" && p.schema>=3 && !nonlinear) {
            row >> p.profile.max_iterations >> p.profile.voltage_tolerance
                >> p.profile.current_tolerance >> p.profile.relative_tolerance;
            nonlinear=true;
        } else if(tag=="wiring" && p.schema>=4 && !wiring) {
            std::string mode; row >> mode;
            if(mode!="nets" && mode!="wires") throw Diagnostic("parse_error","","Expected nets or wires mode");
            p.wired=mode=="wires"; wiring=true;
        } else if(tag=="wire" && p.schema>=4) {
            Wire wire; size_t count=0;
            row >> std::quoted(wire.id) >> std::quoted(wire.from.object) >> std::quoted(wire.from.port)
                >> std::quoted(wire.to.object) >> std::quoted(wire.to.port) >> count;
            if(count>10000) throw Diagnostic("parse_error",wire.id,"Too many wire routing points");
            for(size_t i=0;i<count;++i) { Point point; row >> point.x >> point.y; wire.bends.push_back(point); }
            p.wires.push_back(wire);
        } else if(tag=="pattern" && p.schema>=4) {
            GatePattern pattern; int initial=-1;
            row >> std::quoted(pattern.id) >> std::quoted(pattern.name) >> pattern.x >> pattern.y >> initial;
            if(initial!=0 && initial!=1) row.setstate(std::ios::failbit);
            pattern.initial=initial==1; p.patterns.push_back(pattern);
        } else if(tag=="pwm" && p.schema>=6){
            GatePattern g;g.pwm=true;row>>std::quoted(g.id)>>std::quoted(g.name)>>g.x>>g.y>>g.frequency>>g.duty>>g.delay;p.patterns.push_back(g);
        } else if(tag=="orientation" && p.schema>=6){
            std::string id;Orientation orientation;int mirror=-1;row>>std::quoted(id)>>orientation.quarter_turns>>mirror;
            if(orientation.quarter_turns>3||(mirror!=0&&mirror!=1)||orientations.count(id))row.setstate(std::ios::failbit);
            orientation.mirrored=mirror==1;orientations[id]=orientation;
        } else if(tag=="plot" && p.schema>=5){
            PlotBlock plot;row>>std::quoted(plot.id)>>std::quoted(plot.name)>>plot.x>>plot.y>>plot.inputs>>plot.begin>>plot.end>>plot.cursor_a>>plot.cursor_b;p.plots.push_back(plot);
        } else if(tag=="scope_enabled" && p.schema>=5 && !recording){
            int enabled=-1;row>>enabled;if(enabled!=0&&enabled!=1)row.setstate(std::ios::failbit);p.scope_enabled=enabled==1;recording=true;
        } else if(tag=="scopeview" && p.schema>=4) {
            row >> p.scope_begin >> p.scope_end >> p.cursor_a >> p.cursor_b;
        } else if(tag=="scope" && p.schema>=4) {
            std::string channel; row >> std::quoted(channel); p.scope_channels.push_back(channel);
        } else if(tag=="node") {
            Node n; int g=-1;
            row >> std::quoted(n.id) >> std::quoted(n.name) >> g;
            if(p.schema>=4) row >> n.x >> n.y;
            if(g!=0 && g!=1) row.setstate(std::ios::failbit);
            n.ground=g==1; p.nodes.push_back(n);
        } else if(tag=="component") {
            Component c; std::string kind; int closed=-1;
            row >> std::quoted(c.id) >> std::quoted(c.name) >> kind
                >> std::quoted(c.positive) >> std::quoted(c.negative)
                >> c.value >> c.initial >> c.x >> c.y >> closed;
            if(closed!=0 && closed!=1) row.setstate(std::ios::failbit);
            c.closed=closed==1; c.kind=parse_kind(kind);
            if(c.kind==Kind::diode && p.schema<3) throw Diagnostic("schema_version",c.id,"Diodes require schema 3");
            if((c.kind==Kind::voltage_probe || c.kind==Kind::current_probe) && p.schema<4)
                throw Diagnostic("schema_version",c.id,"Probes require schema 4");
            p.components.push_back(c);
        } else if(tag=="event") {
            GateEvent e{}; int closed=-1;
            row >> e.time >> std::quoted(e.target) >> closed;
            if(closed!=0 && closed!=1) row.setstate(std::ios::failbit);
            e.closed=closed==1; p.events.push_back(e);
        } else throw Diagnostic("parse_error",std::to_string(number),"Unknown or duplicate record: "+tag);
        if(row.fail()) throw Diagnostic("parse_error",std::to_string(number),"Malformed record");
        row >> std::ws;
        if(!row.eof()) throw Diagnostic("parse_error",std::to_string(number),"Trailing fields");
    }
    if(!identity || !profile || (p.schema>=3 && !nonlinear) || (p.schema>=4 && !wiring) || in.bad()) throw Diagnostic("parse_error","","Missing project/profile or read failure");
    // v1 -> v2: default Backward Euler; v2 -> v3: explicit default nonlinear profile.
    for(const auto& [id,orientation]:orientations){
        bool found=false;auto apply=[&](auto& objects){for(auto& object:objects)if(object.id==id){object.orientation=orientation;found=true;}};apply(p.nodes);apply(p.components);apply(p.patterns);apply(p.plots);
        if(!found)throw Diagnostic("missing_orientation_target",id,"Orientation target does not exist");
    }
    p.schema=6;
    return p;
}
void write_project(const Project& p, std::ostream& out) {
    if(p.schema!=6) throw Diagnostic("schema_version",p.id,"Cannot save unsupported schema");
    const auto check_text=[](const std::string& value,const std::string& object) {
        if(value.find_first_of("\r\n")!=std::string::npos)
            throw Diagnostic("invalid_text",object,"Project format strings must be single-line");
    };
    check_text(p.id,p.id); check_text(p.name,p.id);
    for(const auto& node:p.nodes) { check_text(node.id,node.id); check_text(node.name,node.id); }
    for(const auto& c:p.components) {
        check_text(c.id,c.id); check_text(c.name,c.id);
        check_text(c.positive,c.id); check_text(c.negative,c.id);
    }
    for(const auto& event:p.events) check_text(event.target,event.target);
    for(const auto& wire:p.wires) {
        check_text(wire.id,wire.id); check_text(wire.from.object,wire.id); check_text(wire.from.port,wire.id);
        check_text(wire.to.object,wire.id); check_text(wire.to.port,wire.id);
    }
    for(const auto& pattern:p.patterns) { check_text(pattern.id,pattern.id); check_text(pattern.name,pattern.id); }
    for(const auto& channel:p.scope_channels) check_text(channel,p.id);
    for(const auto& plot:p.plots){check_text(plot.id,plot.id);check_text(plot.name,plot.id);}
    out << std::noboolalpha << std::defaultfloat << std::setprecision(17) << "PowerDriveSim 6\nproject " << std::quoted(p.id) << ' ' << std::quoted(p.name)
        << "\nprofile " << p.profile.stop << ' ' << p.profile.step << ' ' << method_name(p.profile.method) << '\n';
    out << "nonlinear " << p.profile.max_iterations << ' ' << p.profile.voltage_tolerance << ' '
        << p.profile.current_tolerance << ' ' << p.profile.relative_tolerance << '\n';
    auto write_orientation=[&](const auto& object){if(object.orientation.quarter_turns>3)throw Diagnostic("invalid_orientation",object.id,"Rotation must contain 0..3 quarter turns");if(object.orientation.quarter_turns||object.orientation.mirrored)out<<"orientation "<<std::quoted(object.id)<<' '<<object.orientation.quarter_turns<<' '<<object.orientation.mirrored<<'\n';};
    for(const auto& c:p.components)write_orientation(c);for(const auto& n:p.nodes)write_orientation(n);for(const auto& g:p.patterns)write_orientation(g);for(const auto& g:p.plots)write_orientation(g);
    out << "wiring " << (p.wired?"wires":"nets") << '\n';
    for(const auto& wire:p.wires) {
        out << "wire " << std::quoted(wire.id) << ' ' << std::quoted(wire.from.object) << ' ' << std::quoted(wire.from.port)
            << ' ' << std::quoted(wire.to.object) << ' ' << std::quoted(wire.to.port) << ' ' << wire.bends.size();
        for(const auto& point:wire.bends) out << ' ' << point.x << ' ' << point.y;
        out << '\n';
    }
    for(const auto& g:p.patterns) if(g.pwm)out<<"pwm "<<std::quoted(g.id)<<' '<<std::quoted(g.name)<<' '<<g.x<<' '<<g.y<<' '<<g.frequency<<' '<<g.duty<<' '<<g.delay<<'\n';else out << "pattern " << std::quoted(g.id) << ' ' << std::quoted(g.name) << ' ' << g.x << ' ' << g.y << ' ' << g.initial << '\n';
    for(const auto& g:p.plots)out<<"plot "<<std::quoted(g.id)<<' '<<std::quoted(g.name)<<' '<<g.x<<' '<<g.y<<' '<<g.inputs<<' '<<g.begin<<' '<<g.end<<' '<<g.cursor_a<<' '<<g.cursor_b<<'\n';
    out<<"scope_enabled "<<p.scope_enabled<<'\n';
    out << "scopeview " << p.scope_begin << ' ' << p.scope_end << ' ' << p.cursor_a << ' ' << p.cursor_b << '\n';
    for(const auto& channel:p.scope_channels) out << "scope " << std::quoted(channel) << '\n';
    for(const auto& n:p.nodes) out << "node " << std::quoted(n.id) << ' ' << std::quoted(n.name) << ' ' << n.ground << ' ' << n.x << ' ' << n.y << '\n';
    for(const auto& c:p.components) out << "component " << std::quoted(c.id) << ' ' << std::quoted(c.name) << ' '
        << kind_name(c.kind) << ' ' << std::quoted(c.positive) << ' ' << std::quoted(c.negative) << ' '
        << c.value << ' ' << c.initial << ' ' << c.x << ' ' << c.y << ' ' << c.closed << '\n';
    for(const auto& e:p.events) out << "event " << e.time << ' ' << std::quoted(e.target) << ' ' << e.closed << '\n';
    for(const auto& e:p.extensions) {
        if(e.rfind("x-",0)!=0 || e.find_first_of("\r\n")!=std::string::npos)
            throw Diagnostic("extension_error",p.id,"Extensions must be single x- records");
        out << e << '\n';
    }
    if(!out) throw Diagnostic("write_error",p.id,"Project write failed");
}
}