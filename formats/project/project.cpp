#include "formats/project/project.hpp"
#include "core/model/hierarchy.hpp"
#include "core/model/waveform.hpp"
#include "core/model/semiconductor.hpp"
#include <iomanip>
#include <map>
#include <istream>
#include <ostream>
#include <sstream>
#include <cmath>
#include <algorithm>
namespace pds {
static Project read_project_impl(std::istream& in,bool definitions_allowed) {
    Project p;
    std::string line, tag;
    if(!std::getline(in,line)) throw Diagnostic("parse_error","","Empty project");
    if(line.compare(0,3,"\xef\xbb\xbf")==0) line.erase(0,3);
    if(!line.empty() && line.back()=='\r') line.pop_back();
    std::istringstream header(line);
    if(!(header >> tag >> p.schema)) throw Diagnostic("parse_error","","Malformed header");
    header >> std::ws;
    if(!header.eof() || tag!="PowerDriveSim" || (p.schema<1 || p.schema>project_schema))
        throw Diagnostic("schema_version","","Expected PowerDriveSim schema 1.."+std::to_string(project_schema));
    bool identity=false, profile=false, nonlinear=false, wiring=false, recording=false, initialization=false, stepping=false;
    std::map<std::string,Orientation> orientations;
    std::map<std::string,SourceWaveform> sources;
    std::map<std::string,Semiconductor> semiconductors;
    struct ChargeData { bool enabled; double transit, lifetime, initial; };
    std::map<std::string,ChargeData> charges;
    std::map<std::string,std::pair<double,bool>> thyristors;
    size_t number=1;
    while(std::getline(in,line)) {
        ++number;
        if(!line.empty() && line.back()=='\r') line.pop_back();
        if(line.empty() || line[0]=='#') continue;
        std::istringstream row(line);
        row >> tag;
        if(tag=="definition"&&p.schema>=7) {
            if(!definitions_allowed)throw Diagnostic("parse_error",p.id,"Definitions must be in the project catalog");
            Definition d;row>>std::quoted(d.id)>>std::quoted(d.name);
            if(row.fail())throw Diagnostic("parse_error",p.id,"Malformed definition");
            row>>std::ws;if(!row.eof())throw Diagnostic("parse_error",d.id,"Trailing definition fields");
            bool body=false,ended=false;std::ostringstream content;
            while(std::getline(in,line)) {
                ++number;if(!line.empty()&&line.back()=='\r')line.pop_back();
                if(line=="end_definition"){ended=true;break;}
                if(body){content<<line<<'\n';continue;}
                if(line=="body"){body=true;continue;}
                if(line.empty()||line.front()=='#')continue;
                std::istringstream meta(line);std::string kind;meta>>kind;
                if(kind=="public_port") {
                    PublicPort port;unsigned domain=0,direction=0;
                    meta>>std::quoted(port.id)>>std::quoted(port.name)>>std::quoted(port.terminal.object)>>std::quoted(port.terminal.port)>>domain>>direction;
                    if(domain>unsigned(Domain::signal)||direction>unsigned(Direction::output))meta.setstate(std::ios::failbit);
                    port.domain=Domain(domain);port.direction=Direction(direction);d.ports.push_back(port);
                } else if(kind=="public_parameter") {
                    PublicParameter param;meta>>std::quoted(param.id)>>std::quoted(param.name)>>std::quoted(param.unit)>>std::quoted(param.object)>>std::quoted(param.field)>>param.value;
                    d.parameters.push_back(param);
                } else throw Diagnostic("parse_error",d.id,"Unknown definition metadata: "+kind);
                if(meta.fail())throw Diagnostic("parse_error",d.id,"Malformed definition metadata");
                meta>>std::ws;if(!meta.eof())throw Diagnostic("parse_error",d.id,"Trailing definition metadata");
            }
            if(!body||!ended)throw Diagnostic("parse_error",d.id,"Unterminated definition body");
            std::istringstream input(content.str());auto nested=read_project_impl(input,false);
            if(nested.id!=d.id||nested.name!=d.name)throw Diagnostic("parse_error",d.id,"Definition body identity mismatch");
            static_cast<Schematic&>(d)=std::move(static_cast<Schematic&>(nested));
            p.definitions.push_back(std::move(d));continue;
        }
        if(tag=="x-view") {
            ViewOptions v;int manual=-1,free=-1,separate=-1,grid=-1,legend=-1;
            row>>std::quoted(v.plot)>>v.y_low>>v.y_high>>manual>>free>>separate>>grid>>legend>>v.line_width>>v.time_span>>std::quoted(v.cursor_channel_a)>>std::quoted(v.cursor_channel_b)>>v.cursor_y_a>>v.cursor_y_b;
            bool parsed=!row.fail();row>>std::ws;
            if(parsed&&!row.eof()) {
                size_t count=0;row>>v.display_columns>>count;
                if(row.fail()||v.display_columns<1||v.display_columns>4||count>10000)throw Diagnostic("parse_error",std::to_string(number),"Invalid display grid");
                for(size_t i=0;i<count;++i){std::string channel;unsigned display;row>>std::quoted(channel)>>display;
                    if(row.fail()||display>15||std::any_of(v.signal_displays.begin(),v.signal_displays.end(),[&](const auto& binding){return binding.first==channel;}))throw Diagnostic("parse_error",std::to_string(number),"Invalid display binding");
                    v.signal_displays.emplace_back(channel,display);
                }
                parsed=!row.fail();row>>std::ws;
            }
            if(parsed&&!row.eof()) {
                size_t count=0;row>>count;
                if(row.fail()||count>10000)throw Diagnostic("parse_error",std::to_string(number),"Invalid hidden channels");
                for(size_t i=0;i<count;++i){std::string channel;row>>std::quoted(channel);
                    if(row.fail()||channel.empty()||std::find(v.hidden_channels.begin(),v.hidden_channels.end(),channel)!=v.hidden_channels.end())throw Diagnostic("parse_error",std::to_string(number),"Invalid hidden channel");
                    v.hidden_channels.push_back(channel);
                }
                parsed=!row.fail();row>>std::ws;
            }
            if(parsed&&!row.eof()) {
                size_t count=0;row>>count;
                if(row.fail()||count>10000)throw Diagnostic("parse_error",std::to_string(number),"Invalid curve styles");
                for(size_t i=0;i<count;++i){CurveStyle style;unsigned line_kind,marker;
                    row>>std::quoted(style.channel)>>line_kind>>style.width>>marker>>style.marker_size;
                    if(row.fail()||style.channel.empty()||line_kind>unsigned(CurveLine::none)||marker>unsigned(CurveMarker::triangle_down)||!std::isfinite(style.width)||style.width<=0||style.width>10||!std::isfinite(style.marker_size)||style.marker_size<1||style.marker_size>24||std::any_of(v.curve_styles.begin(),v.curve_styles.end(),[&](const auto& s){return s.channel==style.channel;}))throw Diagnostic("parse_error",std::to_string(number),"Invalid curve style");
                    style.line=CurveLine(line_kind);style.marker=CurveMarker(marker);v.curve_styles.push_back(style);
                }
                size_t positions=0;row>>positions;
                if(row.fail()||positions>16)throw Diagnostic("parse_error",std::to_string(number),"Invalid legend positions");
                for(size_t i=0;i<positions;++i){LegendPosition pos;row>>pos.display>>pos.x>>pos.y;
                    if(row.fail()||pos.display>15||!std::isfinite(pos.x)||!std::isfinite(pos.y)||pos.x<0||pos.x>1||pos.y<0||pos.y>1||std::any_of(v.legend_positions.begin(),v.legend_positions.end(),[&](const auto& p){return p.display==pos.display;}))throw Diagnostic("parse_error",std::to_string(number),"Invalid legend position");
                    v.legend_positions.push_back(pos);
                }
                parsed=!row.fail();row>>std::ws;
            }
            if(parsed&&!row.eof()) {
                size_t count=0;row>>count;
                if(row.fail()||count>10000)throw Diagnostic("parse_error",std::to_string(number),"Invalid curve names");
                for(size_t i=0;i<count;++i){std::string channel,name;row>>std::quoted(channel)>>std::quoted(name);
                    if(row.fail()||channel.empty()||name.empty()||name.find_first_of("\r\n")!=std::string::npos||std::any_of(v.curve_names.begin(),v.curve_names.end(),[&](const auto& entry){return entry.first==channel;}))throw Diagnostic("parse_error",std::to_string(number),"Invalid curve name");
                    v.curve_names.emplace_back(channel,name);
                }
                parsed=!row.fail();row>>std::ws;
            }
            if(parsed&&!row.eof()) {
                int viewport=-1;row>>viewport>>v.begin>>v.end>>v.cursor_a>>v.cursor_b;
                if(row.fail()||(viewport!=0&&viewport!=1)||!std::isfinite(v.begin)||!std::isfinite(v.end)||!std::isfinite(v.cursor_a)||!std::isfinite(v.cursor_b)||(viewport&&v.end!=-1&&v.end<=v.begin))throw Diagnostic("parse_error",std::to_string(number),"Invalid plot viewport");
                v.viewport=viewport==1;parsed=!row.fail();row>>std::ws;
            }
            if(!parsed||!row.eof()||!std::isfinite(v.y_low)||!std::isfinite(v.y_high)||v.y_low>=v.y_high||!std::isfinite(v.time_span)||v.time_span<0||!std::isfinite(v.line_width)||v.line_width<=0||v.line_width>10||!std::isfinite(v.cursor_y_a)||!std::isfinite(v.cursor_y_b)||manual<0||manual>1||free<0||free>1||separate<0||separate>1||grid<0||grid>1||legend<0||legend>1||std::any_of(p.view_options.begin(),p.view_options.end(),[&](const ViewOptions& o){return o.plot==v.plot;}))throw Diagnostic("parse_error",std::to_string(number),"Invalid view settings");
            v.manual_y=manual;v.free_cursors=free;v.separate_axes=separate;v.grid=grid;v.legend=legend;p.view_options.push_back(v);continue;
        }
        if(tag=="x-label") {
            LabelLayout label;int mirror=-1;
            row>>std::quoted(label.object)>>std::quoted(label.role)>>label.x>>label.y>>label.orientation.quarter_turns>>mirror;
            const bool parsed=!row.fail();row>>std::ws;
            if(!parsed||!row.eof()||!std::isfinite(label.x)||!std::isfinite(label.y)||label.orientation.quarter_turns>3||(mirror!=0&&mirror!=1)||(label.role!="name"&&label.role!="value")||std::any_of(p.labels.begin(),p.labels.end(),[&](const LabelLayout& l){return l.object==label.object&&l.role==label.role;}))throw Diagnostic("parse_error",std::to_string(number),"Invalid label layout");
            label.orientation.mirrored=mirror==1;p.labels.push_back(label);continue;
        }
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
        } else if(tag=="stepping" && p.schema>=14 && !stepping) {
            auto &control = p.profile.step_control;
            int adaptive = -1;
            row >> adaptive >> control.minimum_step >> control.relative_tolerance >> control.voltage_tolerance
                >> control.current_tolerance >> control.charge_tolerance;
            if (adaptive != 0 && adaptive != 1) row.setstate(std::ios::failbit);
            control.adaptive = adaptive == 1;
            stepping = true;
        } else if(tag=="initialization" && p.schema>=13 && !initialization) {
            std::string mode;
            row >> mode >> p.profile.warmup;
            p.profile.initial_state = parse_initial_state(mode);
            initialization = true;
        } else if(tag=="nonlinear" && p.schema>=3 && !nonlinear) {
            row >> p.profile.max_iterations >> p.profile.voltage_tolerance
                >> p.profile.current_tolerance >> p.profile.relative_tolerance;
            nonlinear=true;
        } else if(tag=="wiring" && p.schema>=4 && !wiring) {
            std::string mode; row >> mode;
            if(mode!="nets" && mode!="wires") throw Diagnostic("parse_error","","Expected nets or wires mode");
            p.wired=mode=="wires"; wiring=true;
        } else if(tag=="instance"&&p.schema>=7) {
            Instance instance;size_t count=0;
            row>>std::quoted(instance.id)>>std::quoted(instance.name)>>std::quoted(instance.definition)>>instance.x>>instance.y>>count;
            if(count>10000)throw Diagnostic("parse_error",instance.id,"Too many instance parameters");
            for(size_t i=0;i<count;++i){std::string key;double value=0;row>>std::quoted(key)>>value;instance.parameters.emplace_back(key,value);}
            p.instances.push_back(std::move(instance));
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
            if(c.kind==Kind::igbt && p.schema<12)throw Diagnostic("schema_version",c.id,"IGBT requires schema 12");
            if(c.kind==Kind::thyristor && p.schema<11)throw Diagnostic("schema_version",c.id,"Thyristors require schema 11");
            p.components.push_back(c);
        } else if(tag=="thyristor" && p.schema>=11) {
            std::string id;double holding=0;int latched=-1;
            row>>std::quoted(id)>>holding>>latched;
            if((latched!=0&&latched!=1)||thyristors.count(id))throw Diagnostic("parse_error",id,"Invalid or duplicate thyristor parameters");
            thyristors.emplace(id,std::make_pair(holding,latched==1));
        } else if(tag=="diode_charge" && p.schema>=10) {
            std::string id;ChargeData data{};unsigned enabled=0;
            row>>std::quoted(id)>>enabled>>data.transit>>data.lifetime>>data.initial;
            if(enabled>1||charges.count(id))throw Diagnostic("parse_error",id,"Invalid or duplicate diode charge model");
            data.enabled=enabled!=0;charges.emplace(id,data);
        } else if(tag=="semiconductor" && p.schema>=9) {
            std::string id; Semiconductor s; unsigned model=0;
            row>>std::quoted(id)>>model>>s.ron>>s.roff>>s.forward_voltage;
            if(model>1||semiconductors.count(id))throw Diagnostic("parse_error",id,"Invalid or duplicate semiconductor model");
            s.model=SemiconductorModel(model);semiconductors.emplace(id,s);
        } else if(tag=="source" && p.schema>=8) {
            std::string id; SourceWaveform s; unsigned kind=0;size_t count=0;
            row>>std::quoted(id)>>kind>>s.offset>>s.frequency>>s.phase>>s.delay>>s.duty>>count;
            if(kind>3||count>100000||sources.count(id))throw Diagnostic("parse_error",id,"Invalid or duplicate source waveform");
            s.kind=Waveform(kind);
            for(size_t i=0;i<count;++i){Point point;row>>point.x>>point.y;s.points.push_back(point);}
            sources.emplace(id,std::move(s));
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
        bool found=false;auto apply=[&](auto& objects){for(auto& object:objects)if(object.id==id){object.orientation=orientation;found=true;}};apply(p.nodes);apply(p.components);apply(p.patterns);apply(p.plots);apply(p.instances);
        if(!found)throw Diagnostic("missing_orientation_target",id,"Orientation target does not exist");
    }
    for(const auto& [id,source]:sources) {
        auto c=std::find_if(p.components.begin(),p.components.end(),[&](const auto& c){return c.id==id;});
        if(c==p.components.end())throw Diagnostic("missing_source_target",id,"Source waveform component does not exist");
        c->source=source;validate_waveform(*c);
    }
    for(const auto& [id,model]:semiconductors) {
        auto c=std::find_if(p.components.begin(),p.components.end(),[&](const auto& c){return c.id==id;});
        if(c==p.components.end())throw Diagnostic("missing_semiconductor_target",id,"Semiconductor component does not exist");
        c->semiconductor=model;validate_semiconductor(*c);
    }
    for(const auto& [id,data]:charges) {
        auto c=std::find_if(p.components.begin(),p.components.end(),[&](const auto& c){return c.id==id;});
        if(c==p.components.end()||c->kind!=Kind::diode)throw Diagnostic("invalid_diode_charge",id,"Charge model target must be a diode");
        c->semiconductor.charge_dynamics=data.enabled;c->semiconductor.transit_time=data.transit;
        c->semiconductor.carrier_lifetime=data.lifetime;c->semiconductor.initial_charge=data.initial;
        validate_semiconductor(*c);
    }
    for(const auto& [id,data]:thyristors) {
        auto c=std::find_if(p.components.begin(),p.components.end(),[&](const auto& c){return c.id==id;});
        if(c==p.components.end()||c->kind!=Kind::thyristor)throw Diagnostic("invalid_thyristor",id,"Latch parameters require a thyristor");
        c->semiconductor.holding_current=data.first;c->semiconductor.initial_latched=data.second;
        validate_semiconductor(*c);
    }
    p.schema=project_schema;
    for(const auto& label:p.labels){bool found=false;auto scan=[&](const auto& objects){for(const auto& o:objects)found|=o.id==label.object;};scan(p.components);scan(p.nodes);scan(p.patterns);scan(p.plots);scan(p.instances);if(!found)throw Diagnostic("missing_label_target",label.object,"Label target does not exist");}
    return p;
}
Project read_project(std::istream& in) {
    auto p=read_project_impl(in,true);validate_hierarchy(p);
    auto check_views=[](const Project& project) {
        if(project.view_options.empty())return;
        const auto plots=flatten(project).project.plots;
        for(const auto& v:project.view_options)if(!v.plot.empty()&&std::none_of(plots.begin(),plots.end(),[&](const PlotBlock& plot){return plot.id==v.plot;}))throw Diagnostic("missing_view_target",v.plot,"View target does not exist");
    };
    check_views(p);
    for(const auto& d:p.definitions)check_views(definition_project(p,d.id));
    return p;
}
void write_project(const Project& p, std::ostream& out) {
    if(p.schema!=project_schema) throw Diagnostic("schema_version",p.id,"Cannot save unsupported schema");
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
    for(const auto& instance:p.instances){check_text(instance.id,instance.id);check_text(instance.name,instance.id);check_text(instance.definition,instance.id);for(const auto& [key,value]:instance.parameters){(void)value;check_text(key,instance.id);}}
    for(const auto& v:p.view_options){check_text(v.plot,p.id);check_text(v.cursor_channel_a,p.id);check_text(v.cursor_channel_b,p.id);for(const auto& binding:v.signal_displays)check_text(binding.first,p.id);}
    for(const auto& l:p.labels){check_text(l.object,p.id);check_text(l.role,p.id);}
    out << std::noboolalpha << std::defaultfloat << std::setprecision(17) << "PowerDriveSim " << project_schema << "\nproject " << std::quoted(p.id) << ' ' << std::quoted(p.name)
        << "\nprofile " << p.profile.stop << ' ' << p.profile.step << ' ' << method_name(p.profile.method) << '\n';
    out << "nonlinear " << p.profile.max_iterations << ' ' << p.profile.voltage_tolerance << ' '
        << p.profile.current_tolerance << ' ' << p.profile.relative_tolerance << '\n';
    out << "initialization " << initial_state_name(p.profile.initial_state) << ' ' << p.profile.warmup << '\n';
    const auto &control = p.profile.step_control;
    out << "stepping " << control.adaptive << ' ' << control.minimum_step << ' ' << control.relative_tolerance
        << ' ' << control.voltage_tolerance << ' ' << control.current_tolerance << ' ' << control.charge_tolerance << '\n';
    auto write_orientation=[&](const auto& object){if(object.orientation.quarter_turns>3)throw Diagnostic("invalid_orientation",object.id,"Rotation must contain 0..3 quarter turns");if(object.orientation.quarter_turns||object.orientation.mirrored)out<<"orientation "<<std::quoted(object.id)<<' '<<object.orientation.quarter_turns<<' '<<object.orientation.mirrored<<'\n';};
    for(const auto& c:p.components)write_orientation(c);for(const auto& n:p.nodes)write_orientation(n);for(const auto& g:p.patterns)write_orientation(g);for(const auto& g:p.plots)write_orientation(g);
    for(const auto& i:p.instances)write_orientation(i);
    for(const auto& i:p.instances){out<<"instance "<<std::quoted(i.id)<<' '<<std::quoted(i.name)<<' '<<std::quoted(i.definition)<<' '<<i.x<<' '<<i.y<<' '<<i.parameters.size();for(const auto& [key,value]:i.parameters)out<<' '<<std::quoted(key)<<' '<<value;out<<'\n';}
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
    for(const auto& c:p.components)if(c.semiconductor!=Semiconductor{}) {
        validate_semiconductor(c);const auto& s=c.semiconductor;
        out<<"semiconductor "<<std::quoted(c.id)<<' '<<unsigned(s.model)<<' '<<s.ron<<' '<<s.roff<<' '<<s.forward_voltage<<'\n';
        if(c.kind==Kind::thyristor)
            out<<"thyristor "<<std::quoted(c.id)<<' '<<s.holding_current<<' '<<s.initial_latched<<'\n';
        const Semiconductor defaults;
        if(s.charge_dynamics||s.transit_time!=defaults.transit_time||s.carrier_lifetime!=defaults.carrier_lifetime||s.initial_charge!=0)
            out<<"diode_charge "<<std::quoted(c.id)<<' '<<s.charge_dynamics<<' '<<s.transit_time<<' '<<s.carrier_lifetime<<' '<<s.initial_charge<<'\n';
    }
    for(const auto& c:p.components)if(c.source!=SourceWaveform{}) {
        validate_waveform(c);const auto& s=c.source;
        out<<"source "<<std::quoted(c.id)<<' '<<unsigned(s.kind)<<' '<<s.offset<<' '<<s.frequency<<' '<<s.phase<<' '<<s.delay<<' '<<s.duty<<' '<<s.points.size();
        for(const auto& point:s.points)out<<' '<<point.x<<' '<<point.y;
        out<<'\n';
    }
    for(const auto& l:p.labels)out<<"x-label "<<std::quoted(l.object)<<' '<<std::quoted(l.role)<<' '<<l.x<<' '<<l.y<<' '<<l.orientation.quarter_turns<<' '<<(l.orientation.mirrored?1:0)<<'\n';
    for(const auto& v:p.view_options){out<<"x-view "<<std::quoted(v.plot)<<' '<<v.y_low<<' '<<v.y_high<<' '<<v.manual_y<<' '<<v.free_cursors<<' '<<v.separate_axes<<' '<<v.grid<<' '<<v.legend<<' '<<v.line_width<<' '<<v.time_span<<' '<<std::quoted(v.cursor_channel_a)<<' '<<std::quoted(v.cursor_channel_b)<<' '<<v.cursor_y_a<<' '<<v.cursor_y_b<<' '<<v.display_columns<<' '<<v.signal_displays.size();for(const auto& binding:v.signal_displays)out<<' '<<std::quoted(binding.first)<<' '<<binding.second;out<<' '<<v.hidden_channels.size();for(const auto& channel:v.hidden_channels)out<<' '<<std::quoted(channel);out<<' '<<v.curve_styles.size();for(const auto& style:v.curve_styles)out<<' '<<std::quoted(style.channel)<<' '<<unsigned(style.line)<<' '<<style.width<<' '<<unsigned(style.marker)<<' '<<style.marker_size;out<<' '<<v.legend_positions.size();for(const auto& pos:v.legend_positions)out<<' '<<pos.display<<' '<<pos.x<<' '<<pos.y;out<<' '<<v.curve_names.size();for(const auto& name:v.curve_names)out<<' '<<std::quoted(name.first)<<' '<<std::quoted(name.second);out<<' '<<v.viewport<<' '<<v.begin<<' '<<v.end<<' '<<v.cursor_a<<' '<<v.cursor_b<<'\n';}
    for(const auto& e:p.extensions) {
        if(e.rfind("x-",0)!=0 || e.find_first_of("\r\n")!=std::string::npos)
            throw Diagnostic("extension_error",p.id,"Extensions must be single x- records");
        out << e << '\n';
    }
    for(const auto& d:p.definitions) {
        check_text(d.id,d.id);check_text(d.name,d.id);
        out<<"definition "<<std::quoted(d.id)<<' '<<std::quoted(d.name)<<'\n';
        for(const auto& port:d.ports) {
            for(const auto* value:{&port.id,&port.name,&port.terminal.object,&port.terminal.port})check_text(*value,d.id);
            out<<"public_port "<<std::quoted(port.id)<<' '<<std::quoted(port.name)<<' '<<std::quoted(port.terminal.object)<<' '<<std::quoted(port.terminal.port)<<' '<<unsigned(port.domain)<<' '<<unsigned(port.direction)<<'\n';
        }
        for(const auto& v:d.parameters) {
            for(const auto* value:{&v.id,&v.name,&v.unit,&v.object,&v.field})check_text(*value,d.id);
            out<<"public_parameter "<<std::quoted(v.id)<<' '<<std::quoted(v.name)<<' '<<std::quoted(v.unit)<<' '<<std::quoted(v.object)<<' '<<std::quoted(v.field)<<' '<<v.value<<'\n';
        }
        Project body;static_cast<Schematic&>(body)=d;body.id=d.id;body.name=d.name;
        out<<"body\n";write_project(body,out);out<<"end_definition\n";
    }
    if(!out) throw Diagnostic("write_error",p.id,"Project write failed");
}
}
