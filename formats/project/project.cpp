#include "formats/project/project.hpp"
#include "formats/experiment/experiment.hpp"
#include "core/model/hierarchy.hpp"
#include "core/model/waveform.hpp"
#include "core/model/semiconductor.hpp"
#include <iomanip>
#include <map>
#include <set>
#include <istream>
#include <ostream>
#include <sstream>
#include <cmath>
#include <cctype>
#include <algorithm>
namespace pds {
namespace {
constexpr const char *builtin_three_phase_y = "1a963f2c-ceb8-5cce-b927-44d735ec9e80";
constexpr const char *builtin_three_phase_delta = "eb613164-faf4-5b03-9014-806885fef344";
constexpr const char *builtin_three_phase_kind = "9e07a7ea-8295-5fd0-92c6-6e82c8f1c21b";
constexpr const char *builtin_delta_redundant_source = "158cf373-a245-59e4-89cd-2941c3558c15";
std::string hex_text(const std::string &value) {
    static constexpr char digits[]="0123456789abcdef";
    std::string result;result.reserve(value.size()*2);
    for(const auto byte:value){const auto c=static_cast<unsigned char>(byte);result.push_back(digits[c>>4]);result.push_back(digits[c&15]);}
    return result;
}
std::string unhex_text(const std::string &value,const std::string &object) {
    auto digit=[](char c)->int{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;};
    if(value.size()%2||value.size()>2*1024*1024)throw Diagnostic("parse_error",object,"Invalid encoded expression text");
    std::string result;result.reserve(value.size()/2);
    for(size_t i=0;i<value.size();i+=2){const int high=digit(value[i]),low=digit(value[i+1]);if(high<0||low<0)throw Diagnostic("parse_error",object,"Invalid encoded expression text");result.push_back(char((high<<4)|low));}
    return result;
}
bool parse_complete_quoted_text(const std::string &source,std::string &value) {
    std::istringstream input(source);
    input>>std::quoted(value);
    if(input.fail())return false;
    input>>std::ws;
    return input.eof();
}
bool valid_icon_primitive(const IconPrimitive &primitive) {
    if(unsigned(primitive.kind)>unsigned(IconPrimitiveKind::text) ||
       unsigned(primitive.color)>unsigned(IconColor::accent) ||
       primitive.points.size()>max_icon_points ||
       std::any_of(primitive.points.begin(),primitive.points.end(),[](const Point &point) {
           return !std::isfinite(point.x)||!std::isfinite(point.y)||
                  point.x<0||point.x>32||point.y<0||point.y>32;
       }))
        return false;
    if(primitive.kind==IconPrimitiveKind::text)
        return !primitive.text.empty() && !primitive.points.empty();
    return primitive.points.size()>=2;
}
void migrate_builtin_three_phase_source(Definition &definition) {
    if (definition.id != builtin_three_phase_y && definition.id != builtin_three_phase_delta)
        return;
    if (std::none_of(definition.parameters.begin(), definition.parameters.end(), [](const auto &parameter) {
            return parameter.id == builtin_three_phase_kind || parameter.field == "source_voltage_kind";
        }))
        definition.parameters.push_back({builtin_three_phase_kind, "Voltage kind", "", "*",
                                         "source_voltage_kind",
                                         definition.id == builtin_three_phase_delta ? 1.0 : 0.0});
    if (definition.id != builtin_three_phase_delta)
        return;
    std::erase_if(definition.components, [](const Component &component) {
        return component.id == builtin_delta_redundant_source;
    });
    std::erase_if(definition.wires, [](const Wire &wire) {
        return wire.from.object == builtin_delta_redundant_source ||
               wire.to.object == builtin_delta_redundant_source;
    });
}
} // namespace
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
    bool identity=false, profile=false, nonlinear=false, wiring=false, recording=false, initialization=false, stepping=false,
         expression_initialization=false;
    std::map<std::string,Orientation> orientations;
    std::map<std::string,SourceWaveform> sources;
    std::map<std::string,Semiconductor> semiconductors;
    std::map<std::string,std::pair<bool,double>> parallel_resistances;
    std::map<std::string,std::pair<std::string,std::string>> transformer_secondaries;
    std::map<std::string,MotorParameters> motors;
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
                    if(meta.fail()||domain>unsigned(Domain::signal)||direction>unsigned(Direction::output))
                        throw Diagnostic("parse_error",d.id,"Malformed definition metadata: "+line);
                    std::string tail;
                    std::getline(meta,tail);
                    std::istringstream position(tail);
                    position>>std::ws;
                    if(!position.eof()) {
                        position>>port.x>>port.y;
                        if(position.fail())
                            throw Diagnostic("parse_error",d.id,"Malformed definition metadata: "+line);
                        position.clear();
                        position>>std::ws;
                        if(!position.eof())
                            throw Diagnostic("parse_error",d.id,"Malformed definition metadata: "+line);
                        port.has_position=true;
                    }
                    port.domain=Domain(domain);port.direction=Direction(direction);d.ports.push_back(port);
                    continue;
                } else if(kind=="public_parameter") {
                    PublicParameter param;meta>>std::quoted(param.id)>>std::quoted(param.name)>>std::quoted(param.unit)>>std::quoted(param.object)>>std::quoted(param.field)>>param.value;
                    if(p.schema>=21) {
                        int has_minimum=0,has_maximum=0;
                        meta>>std::quoted(param.group)>>has_minimum>>param.minimum>>has_maximum>>param.maximum;
                        if((has_minimum!=0&&has_minimum!=1)||(has_maximum!=0&&has_maximum!=1))meta.setstate(std::ios::failbit);
                        param.has_minimum=has_minimum==1;param.has_maximum=has_maximum==1;
                    }
                    if(p.schema>=28) {
                        std::string encoded;
                        meta>>std::quoted(encoded);
                        if(!meta.fail())param.default_expression=unhex_text(encoded,param.id);
                    }
                    d.parameters.push_back(param);
                    if(meta.fail())throw Diagnostic("parse_error",d.id,"Malformed definition metadata: "+line);
                    meta>>std::ws;if(!meta.eof())throw Diagnostic("parse_error",d.id,"Trailing definition metadata: "+line);
                    continue;
                } else if(kind=="x-appearance") {
                    meta>>d.appearance.symbol>>std::quoted(d.appearance.image_png);
                    meta>>std::ws;
                    if(meta.fail()||!meta.eof())
                        throw Diagnostic("parse_error",d.id,"Malformed definition appearance");
                    continue;
                } else throw Diagnostic("parse_error",d.id,"Unknown definition metadata: "+kind);
                if(meta.fail())throw Diagnostic("parse_error",d.id,"Malformed definition metadata");
                meta>>std::ws;if(!meta.eof())throw Diagnostic("parse_error",d.id,"Trailing definition metadata");
            }
            if(!body||!ended)throw Diagnostic("parse_error",d.id,"Unterminated definition body");
            std::istringstream input(content.str());auto nested=read_project_impl(input,false);
            if(nested.id!=d.id||nested.name!=d.name)throw Diagnostic("parse_error",d.id,"Definition body identity mismatch");
            static_cast<Schematic&>(d)=std::move(static_cast<Schematic&>(nested));
            migrate_builtin_three_phase_source(d);
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
            if(parsed&&!row.eof()) {
                size_t count=0;row>>count;
                if(row.fail()||count>10000)throw Diagnostic("parse_error",std::to_string(number),"Invalid curve multipliers");
                for(size_t i=0;i<count;++i){std::string channel;double multiplier=1;row>>std::quoted(channel)>>multiplier;
                    if(row.fail()||channel.empty()||!std::isfinite(multiplier)||std::any_of(v.curve_multipliers.begin(),v.curve_multipliers.end(),[&](const auto& entry){return entry.first==channel;}))throw Diagnostic("parse_error",std::to_string(number),"Invalid curve multiplier");
                    if(std::abs(multiplier-1.0)>1e-15)v.curve_multipliers.emplace_back(channel,multiplier);
                }
                parsed=!row.fail();row>>std::ws;
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
        if(tag=="x-plot-pin") {
            std::string plot_id;PinPosition pin;
            row>>std::quoted(plot_id)>>std::quoted(pin.port)>>pin.x>>pin.y;
            const bool parsed=!row.fail();row>>std::ws;
            auto plot=std::find_if(p.plots.begin(),p.plots.end(),[&](const PlotBlock& candidate){return candidate.id==plot_id;});
            if(!parsed||!row.eof()||plot==p.plots.end()||pin.port.empty()||!std::isfinite(pin.x)||!std::isfinite(pin.y)||
               std::any_of(plot->pin_positions.begin(),plot->pin_positions.end(),[&](const PinPosition& prior){return prior.port==pin.port;}))
                throw Diagnostic("parse_error",std::to_string(number),"Invalid plot pin layout");
            plot->pin_positions.push_back(std::move(pin));continue;
        }
        if(tag=="x-gate-pin" && p.schema>=25) {
            std::string gate_id;PinPosition pin;
            row>>std::quoted(gate_id)>>std::quoted(pin.port)>>pin.x>>pin.y;
            const bool parsed=!row.fail();row>>std::ws;
            auto gate=std::find_if(p.patterns.begin(),p.patterns.end(),[&](const GatePattern& candidate){return candidate.id==gate_id;});
            auto valid_port=[&] {
                if(gate==p.patterns.end())return false;
                if(pin.port=="out")return gate->outputs>0;
                if(pin.port.rfind("out",0)!=0)return false;
                try{return std::stoul(pin.port.substr(3))<gate->outputs;}catch(...){return false;}
            };
            if(!parsed||!row.eof()||!valid_port()||!std::isfinite(pin.x)||!std::isfinite(pin.y)||
               std::any_of(gate->pin_positions.begin(),gate->pin_positions.end(),[&](const PinPosition& prior){return prior.port==pin.port;}))
                throw Diagnostic("parse_error",std::to_string(number),"Invalid Gate pin layout");
            gate->pin_positions.push_back(std::move(pin));continue;
        }
        if(tag=="x-code-pin" && p.schema>=23) {
            std::string block_id;PinPosition pin;
            row>>std::quoted(block_id)>>std::quoted(pin.port)>>pin.x>>pin.y;
            const bool parsed=!row.fail();row>>std::ws;
            auto block=std::find_if(p.code_blocks.begin(),p.code_blocks.end(),[&](const CodeBlock &candidate){return candidate.id==block_id;});
            if(!parsed||!row.eof()||block==p.code_blocks.end()||pin.port.empty()||!std::isfinite(pin.x)||!std::isfinite(pin.y)||
               std::any_of(block->pin_positions.begin(),block->pin_positions.end(),[&](const PinPosition &prior){return prior.port==pin.port;}))
                throw Diagnostic("parse_error",std::to_string(number),"Invalid code-block pin layout");
            block->pin_positions.push_back(std::move(pin));continue;
        }
        if(tag=="x-code-icon" && p.schema>=26) {
            std::string block_id,encoded;unsigned kind=0,color=0;int filled=-1;size_t count=0;
            row>>std::quoted(block_id)>>kind>>color>>filled>>std::quoted(encoded)>>count;
            IconPrimitive primitive;
            primitive.kind=IconPrimitiveKind(kind);primitive.color=IconColor(color);primitive.filled=filled==1;
            primitive.text=unhex_text(encoded,block_id);
            if(count>max_icon_points)row.setstate(std::ios::failbit);
            for(size_t index=0;index<count&&!row.fail();++index) {
                Point point;row>>point.x>>point.y;primitive.points.push_back(point);
            }
            const bool parsed=!row.fail();row>>std::ws;
            auto block=std::find_if(p.code_blocks.begin(),p.code_blocks.end(),[&](const CodeBlock &candidate){return candidate.id==block_id;});
            if(!parsed||!row.eof()||block==p.code_blocks.end()||(filled!=0&&filled!=1)||
               block->icon.size()>=max_icon_primitives||!valid_icon_primitive(primitive))
                throw Diagnostic("parse_error",std::to_string(number),"Invalid code-block icon primitive");
            block->icon.push_back(std::move(primitive));continue;
        }
        if(tag=="x-object-icon" && p.schema>=27) {
            std::string object,encoded;unsigned kind=0,color=0;int filled=-1;size_t count=0;
            row>>std::quoted(object)>>kind>>color>>filled>>std::quoted(encoded)>>count;
            IconPrimitive primitive;
            primitive.kind=IconPrimitiveKind(kind);primitive.color=IconColor(color);primitive.filled=filled==1;
            primitive.text=unhex_text(encoded,object);
            if(count>max_icon_points)row.setstate(std::ios::failbit);
            for(size_t index=0;index<count&&!row.fail();++index) {
                Point point;row>>point.x>>point.y;primitive.points.push_back(point);
            }
            const bool parsed=!row.fail();row>>std::ws;
            auto appearance=std::find_if(p.object_icons.begin(),p.object_icons.end(),[&](const ObjectIcon &candidate){return candidate.object==object;});
            if(appearance==p.object_icons.end()) {
                p.object_icons.push_back({object,{}});appearance=std::prev(p.object_icons.end());
            }
            if(!parsed||!row.eof()||object.empty()||(filled!=0&&filled!=1)||
               appearance->primitives.size()>=max_icon_primitives||!valid_icon_primitive(primitive))
                throw Diagnostic("parse_error",std::to_string(number),"Invalid object icon primitive");
            appearance->primitives.push_back(std::move(primitive));continue;
        }
        if(tag.rfind("x-",0)==0) { p.extensions.push_back(line); continue; }
        if(tag=="project" && !identity) {
            row >> std::quoted(p.id) >> std::quoted(p.name); identity=true;
        } else if(tag=="expression_init" && p.schema>=22 && !expression_initialization) {
            std::string encoded;row>>std::quoted(encoded);p.initialization_code=unhex_text(encoded,p.id);expression_initialization=true;
        } else if(tag=="parameter_expression" && p.schema>=22) {
            ParameterExpression expression;std::string encoded;
            row>>std::quoted(expression.object)>>std::quoted(expression.field)>>std::quoted(encoded);
            expression.source=unhex_text(encoded,expression.object);
            if(expression.object.empty()||expression.field.empty()||expression.source.empty()||
               std::any_of(p.parameter_expressions.begin(),p.parameter_expressions.end(),[&](const auto &prior){return prior.object==expression.object&&prior.field==expression.field;}))
                row.setstate(std::ios::failbit);
            p.parameter_expressions.push_back(std::move(expression));
        } else if(tag=="profile" && !profile) {
            row >> p.profile.stop >> p.profile.step;
            if(p.schema>=2) {
                std::string method;
                if(!(row >> method)) throw Diagnostic("parse_error",std::to_string(number),"Missing integration method");
                p.profile.method=parse_method(method);
            }
            profile=true;
        } else if(tag=="experiment" && p.schema>=15 && definitions_allowed) {
            auto experiment = read_experiment(row);
            if(std::any_of(p.experiments.begin(),p.experiments.end(),[&](const auto& e){return e.id==experiment.id;}))
                throw Diagnostic("invalid_uuid",experiment.id,"Duplicate experiment UUID");
            p.experiments.push_back(std::move(experiment));
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
            if(p.schema>=16) {
                row>>std::ws;
                if(!row.eof()) {
                    int locked=-1;row>>locked;
                    if(locked!=0&&locked!=1)row.setstate(std::ios::failbit);
                    instance.locked=locked==1;
                }
            }
            p.instances.push_back(std::move(instance));
        } else if(tag=="wire" && p.schema>=4) {
            Wire wire; size_t count=0;
            row >> std::quoted(wire.id) >> std::quoted(wire.from.object) >> std::quoted(wire.from.port)
                >> std::quoted(wire.to.object) >> std::quoted(wire.to.port) >> count;
            if(count>10000) throw Diagnostic("parse_error",wire.id,"Too many wire routing points");
            for(size_t i=0;i<count;++i) { Point point; row >> point.x >> point.y; wire.bends.push_back(point); }
            if (p.schema >= 19) {
                unsigned line_style = 0;
                row >> std::quoted(wire.color) >> wire.width >> line_style;
                const bool invalid_color =
                    !wire.color.empty() &&
                    (wire.color.size() != 7 || wire.color[0] != '#' ||
                     !std::all_of(wire.color.begin() + 1, wire.color.end(),
                                  [](unsigned char c) { return std::isxdigit(c) != 0; }));
                if (invalid_color || line_style > unsigned(WireLine::dash) || !std::isfinite(wire.width) ||
                    wire.width < .5 || wire.width > 10)
                    row.setstate(std::ios::failbit);
                wire.line = WireLine(line_style);
            }
            p.wires.push_back(wire);
        } else if(tag=="tag" && p.schema>=16) {
            ConnectionTag t;unsigned domain=0,scope=0;int listed=1;
            row>>std::quoted(t.id)>>std::quoted(t.name)>>t.x>>t.y>>domain;
            if(row>>scope) {
                if(!(row>>listed))row.setstate(std::ios::failbit);
            } else if(row.eof()) {
                // Schema 16..20 records end after the domain. An attempted
                // optional read reaches EOF; clear it so the shared trailing
                // field check can process the legacy record normally.
                row.clear();
            }
            if(domain>unsigned(Domain::signal)||scope>unsigned(TagScope::global)||(listed!=0&&listed!=1))row.setstate(std::ios::failbit);
            t.domain=Domain(domain);t.scope=TagScope(scope);t.listed=listed==1;p.tags.push_back(t);
        } else if(tag=="pattern" && p.schema>=4) {
            GatePattern pattern; int initial=-1;
            row >> std::quoted(pattern.id) >> std::quoted(pattern.name) >> pattern.x >> pattern.y >> initial;
            if(initial!=0 && initial!=1) row.setstate(std::ios::failbit);
            pattern.initial=initial==1; p.patterns.push_back(pattern);
        } else if(tag=="pwm" && p.schema>=6){
            GatePattern g;g.pwm=true;row>>std::quoted(g.id)>>std::quoted(g.name)>>g.x>>g.y>>g.frequency>>g.duty>>g.delay;p.patterns.push_back(g);
        } else if(tag=="gate_script" && p.schema>=16){
            GatePattern g;g.script=true;
            row>>std::quoted(g.id)>>std::quoted(g.name)>>g.x>>g.y>>g.initial>>g.script_step;
            if(p.schema>=23) {
                std::string encoded;row>>std::quoted(encoded);g.code=unhex_text(encoded,g.id);
                if(p.schema>=24)row>>g.outputs;
            } else if(!row.fail()) {
                // Versions 16..22 wrote formatted C source directly. Recover scripts
                // whose physical newlines accidentally split one project record.
                std::string quoted_source;
                std::getline(row,quoted_source);
                while(!parse_complete_quoted_text(quoted_source,g.code)) {
                    std::string continuation;
                    if(!std::getline(in,continuation)||quoted_source.size()+continuation.size()+1>1024*1024) {
                        row.setstate(std::ios::failbit);
                        break;
                    }
                    ++number;
                    quoted_source+='\n';
                    quoted_source+=continuation;
                }
            }
            p.patterns.push_back(std::move(g));
        } else if(tag=="orientation" && p.schema>=6){
            std::string id;Orientation orientation;int mirror=-1;row>>std::quoted(id)>>orientation.quarter_turns>>mirror;
            std::vector<double> values;
            std::string rest;
            std::getline(row, rest);
            if (row.fail() && row.eof())
                row.clear();
            std::istringstream tail(rest);
            for (double value = 0; tail >> value;)
                values.push_back(value);
            if (tail.fail() && !tail.eof())
                row.setstate(std::ios::failbit);
            if (p.schema >= 17 || !values.empty()) {
                if (values.size() == 1) {
                    orientation.scale = values[0];
                } else if (values.size() == 3) {
                    orientation.scale = values[0];
                    orientation.scale_x = values[1];
                    orientation.scale_y = values[2];
                } else if (!values.empty()) {
                    row.setstate(std::ios::failbit);
                }
            }
            if(orientation.quarter_turns>3||(mirror!=0&&mirror!=1)||!std::isfinite(orientation.scale)||orientation.scale<=0||
               !std::isfinite(orientation.scale_x)||orientation.scale_x<=0||!std::isfinite(orientation.scale_y)||orientation.scale_y<=0||
               orientations.count(id))row.setstate(std::ios::failbit);
            orientation.mirrored=mirror==1;orientations[id]=orientation;
        } else if(tag=="plot" && p.schema>=5){
            PlotBlock plot;row>>std::quoted(plot.id)>>std::quoted(plot.name)>>plot.x>>plot.y>>plot.inputs>>plot.begin>>plot.end>>plot.cursor_a>>plot.cursor_b;
            if(p.schema>=20){int differential=-1;row>>differential;if(differential!=0&&differential!=1)row.setstate(std::ios::failbit);plot.differential=differential==1;}
            p.plots.push_back(plot);
        } else if(tag=="code_block" && p.schema>=23) {
            CodeBlock block;
            std::string encoded;
            row>>std::quoted(block.id)>>std::quoted(block.name)>>block.x>>block.y>>block.period>>block.phase>>std::quoted(encoded);
            block.code=unhex_text(encoded,block.id);
            p.code_blocks.push_back(std::move(block));
        } else if((tag=="code_input"||tag=="code_output") && p.schema>=23) {
            std::string block_id;
            CodePort port;
            unsigned type=0;
            row>>std::quoted(block_id)>>std::quoted(port.id)>>std::quoted(port.name)>>std::quoted(port.unit)>>type>>port.initial;
            auto block=std::find_if(p.code_blocks.begin(),p.code_blocks.end(),[&](const CodeBlock &candidate){return candidate.id==block_id;});
            if(block==p.code_blocks.end()||type>unsigned(SignalScalarType::boolean))row.setstate(std::ios::failbit);
            else {
                port.type=SignalScalarType(type);
                (tag=="code_input"?block->inputs:block->outputs).push_back(std::move(port));
            }
        } else if(tag=="scope_enabled" && p.schema>=5 && !recording){
            int enabled=-1;row>>enabled;if(enabled!=0&&enabled!=1)row.setstate(std::ios::failbit);p.scope_enabled=enabled==1;recording=true;
        } else if(tag=="scopeview" && p.schema>=4) {
            row >> p.scope_begin >> p.scope_end >> p.cursor_a >> p.cursor_b;
        } else if(tag=="scope" && p.schema>=4) {
            std::string channel; row >> std::quoted(channel); p.scope_channels.push_back(channel);
        } else if(tag=="scope_point" && p.schema>=18) {
            std::string channel; row >> std::quoted(channel); p.scope_points.push_back(channel);
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
            if(c.kind==Kind::ideal_transformer && p.schema<29)throw Diagnostic("schema_version",c.id,"Ideal transformers require schema 29");
            if(c.kind==Kind::dc_motor && p.schema<30)throw Diagnostic("schema_version",c.id,"DC motors require schema 30");
            p.components.push_back(c);
        } else if(tag=="dc_motor" && p.schema>=30) {
            std::string id;MotorParameters model;
            row>>std::quoted(id)>>model.torque_constant>>model.back_emf_constant
               >>model.inertia>>model.damping>>model.load_torque;
            if(!motors.emplace(id,model).second)
                throw Diagnostic("parse_error",id,"Duplicate DC motor parameters");
        } else if(tag=="transformer_secondary" && p.schema>=29) {
            std::string id,positive,negative;
            row>>std::quoted(id)>>std::quoted(positive)>>std::quoted(negative);
            if(!transformer_secondaries.emplace(id,std::make_pair(positive,negative)).second)
                throw Diagnostic("parse_error",id,"Duplicate transformer secondary terminals");
        } else if(tag=="parallel_resistance" && p.schema>=17) {
            std::string id;int enabled=-1;double resistance=0;
            row>>std::quoted(id)>>enabled>>resistance;
            if((enabled!=0&&enabled!=1)||!std::isfinite(resistance)||resistance<=0||parallel_resistances.count(id))
                throw Diagnostic("parse_error",id,"Invalid or duplicate parallel resistance");
            parallel_resistances.emplace(id,std::make_pair(enabled==1,resistance));
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
        bool found=false;auto apply=[&](auto& objects){for(auto& object:objects)if(object.id==id){object.orientation=orientation;found=true;}};apply(p.nodes);apply(p.components);apply(p.tags);apply(p.patterns);apply(p.plots);apply(p.code_blocks);apply(p.instances);
        if(!found)throw Diagnostic("missing_orientation_target",id,"Orientation target does not exist");
    }
    for(const auto &appearance:p.object_icons) {
        bool found=false;auto scan=[&](const auto &objects){for(const auto &object:objects)found|=object.id==appearance.object;};
        scan(p.components);scan(p.patterns);scan(p.plots);scan(p.code_blocks);scan(p.instances);scan(p.tags);
        if(!found)throw Diagnostic("missing_icon_target",appearance.object,"Object icon target does not exist");
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
    for(const auto& [id,data]:parallel_resistances) {
        auto c=std::find_if(p.components.begin(),p.components.end(),[&](const auto& c){return c.id==id;});
        if(c==p.components.end()||c->kind!=Kind::inductor)
            throw Diagnostic("invalid_parameter",id,"Parallel resistance target must be an inductor");
        c->parallel_resistance_enabled=data.first;c->parallel_resistance=data.second;
    }
    for(const auto& [id,terminals]:transformer_secondaries) {
        auto c=std::find_if(p.components.begin(),p.components.end(),[&](const auto& component){return component.id==id;});
        if(c==p.components.end()||c->kind!=Kind::ideal_transformer)
            throw Diagnostic("invalid_parameter",id,"Secondary terminals require an ideal transformer");
        c->secondary_positive=terminals.first;
        c->secondary_negative=terminals.second;
    }
    for(const auto& c:p.components)
        if(c.kind==Kind::ideal_transformer&&!transformer_secondaries.contains(c.id))
            throw Diagnostic("missing_terminal",c.id,"Transformer secondary terminals are missing");
    for(const auto& [id,model]:motors) {
        auto c=std::find_if(p.components.begin(),p.components.end(),[&](const auto& component){return component.id==id;});
        if(c==p.components.end()||c->kind!=Kind::dc_motor)
            throw Diagnostic("invalid_parameter",id,"Motor parameters require a DC motor");
        c->motor=model;
        validate_motor(*c);
    }
    for(const auto& c:p.components)
        if(c.kind==Kind::dc_motor&&!motors.contains(c.id))
            throw Diagnostic("invalid_parameter",c.id,"DC motor parameters are missing");
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
    if(p.schema<18)p.scope_points=p.scope_channels;
    p.schema=project_schema;
    for(const auto& label:p.labels){bool found=false;auto scan=[&](const auto& objects){for(const auto& o:objects)found|=o.id==label.object;};scan(p.components);scan(p.nodes);scan(p.tags);scan(p.patterns);scan(p.plots);scan(p.code_blocks);scan(p.instances);if(!found)throw Diagnostic("missing_label_target",label.object,"Label target does not exist");}
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
    std::set<std::string> experiment_ids;
    for(const auto& e:p.experiments) {
        (void)experiment_size(e);
        if(!experiment_ids.insert(e.id).second)throw Diagnostic("invalid_uuid",e.id,"Duplicate experiment UUID");
    }
    for(const auto& node:p.nodes) { check_text(node.id,node.id); check_text(node.name,node.id); }
    for(const auto& c:p.components) {
        check_text(c.id,c.id); check_text(c.name,c.id);
        check_text(c.positive,c.id); check_text(c.negative,c.id);
    }
    for(const auto& event:p.events) check_text(event.target,event.target);
    for(const auto& wire:p.wires) {
        check_text(wire.id,wire.id); check_text(wire.from.object,wire.id); check_text(wire.from.port,wire.id);
        check_text(wire.to.object,wire.id); check_text(wire.to.port,wire.id);
        check_text(wire.color, wire.id);
    }
    for(const auto& tag:p.tags) { check_text(tag.id,tag.id); check_text(tag.name,tag.id); }
    for(const auto& pattern:p.patterns) {
        check_text(pattern.id,pattern.id);check_text(pattern.name,pattern.id);std::set<std::string> pins;
        for(const auto& pin:pattern.pin_positions) {
            check_text(pin.port,pattern.id);
            bool valid=pin.port=="out";
            if(!valid&&pin.port.rfind("out",0)==0)
                try{valid=std::stoul(pin.port.substr(3))<pattern.outputs;}catch(...){valid=false;}
            if(!valid||!std::isfinite(pin.x)||!std::isfinite(pin.y)||!pins.insert(pin.port).second)
                throw Diagnostic("invalid_gate_pin",pattern.id,"Gate pin layout is invalid");
        }
    }
    for(const auto& channel:p.scope_points) check_text(channel,p.id);
    for(const auto& channel:p.scope_channels) check_text(channel,p.id);
    for(const auto& plot:p.plots){
        check_text(plot.id,plot.id);check_text(plot.name,plot.id);std::set<std::string> pins;
        for(const auto& pin:plot.pin_positions) {
            check_text(pin.port,plot.id);
            bool valid=false;
            for(unsigned i=1;i<=plot.inputs;++i)
                valid|=pin.port==(plot.differential?"p"+std::to_string(i):"in"+std::to_string(i))||
                       (plot.differential&&pin.port=="n"+std::to_string(i));
            if(!valid||!std::isfinite(pin.x)||!std::isfinite(pin.y)||!pins.insert(pin.port).second)
                throw Diagnostic("invalid_plot_pin",plot.id,"Plot pin layout is invalid");
        }
    }
    for(const auto& block:p.code_blocks) {
        check_text(block.id,block.id);check_text(block.name,block.id);
        if(!valid_uuid(block.id)||block.code.empty()||block.code.size()>1024*1024||!std::isfinite(block.x)||!std::isfinite(block.y)||
           !std::isfinite(block.period)||block.period<=0||!std::isfinite(block.phase)||block.phase<0||
           block.inputs.size()>32||block.outputs.empty()||block.outputs.size()>32)
            throw Diagnostic("invalid_code_block",block.id,"Code block geometry, schedule, source or port count is invalid");
        std::set<std::string> port_ids,names;
        auto check_port=[&](const CodePort &port) {
            check_text(port.id,block.id);check_text(port.name,block.id);check_text(port.unit,block.id);
            if(!valid_uuid(port.id)||!port_ids.insert(port.id).second||port.name.empty()||!names.insert(port.name).second||
               unsigned(port.type)>unsigned(SignalScalarType::boolean)||
               !std::isfinite(port.initial)||(port.type==SignalScalarType::boolean&&port.initial!=0&&port.initial!=1))
                throw Diagnostic("invalid_signal_port",port.id,"Code block port is invalid or duplicated");
        };
        for(const auto& port:block.inputs)check_port(port);
        for(const auto& port:block.outputs)check_port(port);
        std::set<std::string> positioned;
        for(const auto& pin:block.pin_positions)
            if(!port_ids.contains(pin.port)||!positioned.insert(pin.port).second||!std::isfinite(pin.x)||!std::isfinite(pin.y))
                throw Diagnostic("invalid_signal_port",block.id,"Code block pin layout is invalid");
        if(block.icon.size()>max_icon_primitives)
            throw Diagnostic("invalid_code_icon",block.id,"Code block icon has too many primitives");
        for(const auto& primitive:block.icon) {
            check_text(primitive.text,block.id);
            if(!valid_icon_primitive(primitive))
                throw Diagnostic("invalid_code_icon",block.id,"Code block icon primitive is incomplete");
        }
    }
    std::set<std::string> icon_targets;
    auto collect_icon_targets=[&](const auto &objects){for(const auto &object:objects)icon_targets.insert(object.id);};
    collect_icon_targets(p.components);collect_icon_targets(p.patterns);collect_icon_targets(p.plots);
    collect_icon_targets(p.code_blocks);collect_icon_targets(p.instances);collect_icon_targets(p.tags);
    std::set<std::string> decorated;
    for(const auto &appearance:p.object_icons) {
        check_text(appearance.object,p.id);
        if(!icon_targets.contains(appearance.object)||!decorated.insert(appearance.object).second||appearance.primitives.empty()||appearance.primitives.size()>max_icon_primitives)
            throw Diagnostic("invalid_object_icon",appearance.object,"Object icon target or primitive count is invalid");
        for(const auto &primitive:appearance.primitives) {
            check_text(primitive.text,appearance.object);
            if(!valid_icon_primitive(primitive))
                throw Diagnostic("invalid_object_icon",appearance.object,"Object icon primitive is invalid");
        }
    }
    for(const auto& instance:p.instances){check_text(instance.id,instance.id);check_text(instance.name,instance.id);check_text(instance.definition,instance.id);for(const auto& [key,value]:instance.parameters){(void)value;check_text(key,instance.id);}}
    for(const auto& v:p.view_options){check_text(v.plot,p.id);check_text(v.cursor_channel_a,p.id);check_text(v.cursor_channel_b,p.id);for(const auto& binding:v.signal_displays)check_text(binding.first,p.id);for(const auto& multiplier:v.curve_multipliers)check_text(multiplier.first,p.id);}
    for(const auto& l:p.labels){check_text(l.object,p.id);check_text(l.role,p.id);}
    if(p.initialization_code.size()>1024*1024)throw Diagnostic("invalid_initialization",p.id,"Initialization code exceeds 1 MiB");
    std::set<std::pair<std::string,std::string>> expression_bindings;
    for(const auto& expression:p.parameter_expressions) {
        check_text(expression.object,expression.object);check_text(expression.field,expression.object);
        if(expression.source.empty()||expression.source.size()>1024*1024||
           !expression_bindings.emplace(expression.object,expression.field).second)
            throw Diagnostic("invalid_parameter_expression",expression.object,"Parameter expression is empty, too large or duplicated");
    }
    out << std::noboolalpha << std::defaultfloat << std::setprecision(17) << "PowerDriveSim " << project_schema << "\nproject " << std::quoted(p.id) << ' ' << std::quoted(p.name)
        << "\nprofile " << p.profile.stop << ' ' << p.profile.step << ' ' << method_name(p.profile.method) << '\n';
    out << "nonlinear " << p.profile.max_iterations << ' ' << p.profile.voltage_tolerance << ' '
        << p.profile.current_tolerance << ' ' << p.profile.relative_tolerance << '\n';
    out << "initialization " << initial_state_name(p.profile.initial_state) << ' ' << p.profile.warmup << '\n';
    const auto &control = p.profile.step_control;
    out << "stepping " << control.adaptive << ' ' << control.minimum_step << ' ' << control.relative_tolerance
        << ' ' << control.voltage_tolerance << ' ' << control.current_tolerance << ' ' << control.charge_tolerance << '\n';
    if(!p.initialization_code.empty())out<<"expression_init "<<std::quoted(hex_text(p.initialization_code))<<'\n';
    for(const auto& expression:p.parameter_expressions)
        out<<"parameter_expression "<<std::quoted(expression.object)<<' '<<std::quoted(expression.field)<<' '<<std::quoted(hex_text(expression.source))<<'\n';
    auto write_orientation=[&](const auto& object){const auto& o=object.orientation;if(o.quarter_turns>3||!std::isfinite(o.scale)||o.scale<=0||!std::isfinite(o.scale_x)||o.scale_x<=0||!std::isfinite(o.scale_y)||o.scale_y<=0)throw Diagnostic("invalid_orientation",object.id,"Invalid orientation");if(o.quarter_turns||o.mirrored||std::abs(o.scale-1)>1e-12||std::abs(o.scale_x-1)>1e-12||std::abs(o.scale_y-1)>1e-12)out<<"orientation "<<std::quoted(object.id)<<' '<<o.quarter_turns<<' '<<o.mirrored<<' '<<o.scale<<' '<<o.scale_x<<' '<<o.scale_y<<'\n';};
    for(const auto& experiment:p.experiments) { out<<"experiment ";write_experiment(out,experiment);out<<'\n'; }
    for(const auto& c:p.components)write_orientation(c);
    for(const auto& n:p.nodes)write_orientation(n);
    for(const auto& t:p.tags)write_orientation(t);
    for(const auto& g:p.patterns)write_orientation(g);
    for(const auto& g:p.plots)write_orientation(g);
    for(const auto& g:p.code_blocks)write_orientation(g);
    for(const auto& i:p.instances)write_orientation(i);
    for(const auto& i:p.instances){out<<"instance "<<std::quoted(i.id)<<' '<<std::quoted(i.name)<<' '<<std::quoted(i.definition)<<' '<<i.x<<' '<<i.y<<' '<<i.parameters.size();for(const auto& [key,value]:i.parameters)out<<' '<<std::quoted(key)<<' '<<value;out<<' '<<i.locked<<'\n';}
    out << "wiring " << (p.wired?"wires":"nets") << '\n';
    for(const auto& wire:p.wires) {
        out << "wire " << std::quoted(wire.id) << ' ' << std::quoted(wire.from.object) << ' ' << std::quoted(wire.from.port)
            << ' ' << std::quoted(wire.to.object) << ' ' << std::quoted(wire.to.port) << ' ' << wire.bends.size();
        for(const auto& point:wire.bends) out << ' ' << point.x << ' ' << point.y;
        if (!wire.color.empty() &&
            (wire.color.size() != 7 || wire.color[0] != '#' ||
             !std::all_of(wire.color.begin() + 1, wire.color.end(),
                          [](unsigned char c) { return std::isxdigit(c) != 0; })))
            throw Diagnostic("invalid_wire_style", wire.id, "Invalid wire color");
        if (!std::isfinite(wire.width) || wire.width < .5 || wire.width > 10 ||
            unsigned(wire.line) > unsigned(WireLine::dash))
            throw Diagnostic("invalid_wire_style", wire.id, "Invalid wire style");
        out << ' ' << std::quoted(wire.color) << ' ' << wire.width << ' ' << unsigned(wire.line) << '\n';
    }
    for(const auto& tag:p.tags)out<<"tag "<<std::quoted(tag.id)<<' '<<std::quoted(tag.name)<<' '<<tag.x<<' '<<tag.y<<' '<<unsigned(tag.domain)<<' '<<unsigned(tag.scope)<<' '<<tag.listed<<'\n';
    for(const auto& g:p.patterns) if(g.script)out<<"gate_script "<<std::quoted(g.id)<<' '<<std::quoted(g.name)<<' '<<g.x<<' '<<g.y<<' '<<g.initial<<' '<<g.script_step<<' '<<std::quoted(hex_text(g.code))<<' '<<g.outputs<<'\n';else if(g.pwm)out<<"pwm "<<std::quoted(g.id)<<' '<<std::quoted(g.name)<<' '<<g.x<<' '<<g.y<<' '<<g.frequency<<' '<<g.duty<<' '<<g.delay<<'\n';else out << "pattern " << std::quoted(g.id) << ' ' << std::quoted(g.name) << ' ' << g.x << ' ' << g.y << ' ' << g.initial << '\n';
    for(const auto& g:p.patterns)for(const auto& pin:g.pin_positions)out<<"x-gate-pin "<<std::quoted(g.id)<<' '<<std::quoted(pin.port)<<' '<<pin.x<<' '<<pin.y<<'\n';
    for(const auto& g:p.plots)out<<"plot "<<std::quoted(g.id)<<' '<<std::quoted(g.name)<<' '<<g.x<<' '<<g.y<<' '<<g.inputs<<' '<<g.begin<<' '<<g.end<<' '<<g.cursor_a<<' '<<g.cursor_b<<' '<<g.differential<<'\n';
    for(const auto& g:p.plots)for(const auto& pin:g.pin_positions)out<<"x-plot-pin "<<std::quoted(g.id)<<' '<<std::quoted(pin.port)<<' '<<pin.x<<' '<<pin.y<<'\n';
    for(const auto& block:p.code_blocks) {
        out<<"code_block "<<std::quoted(block.id)<<' '<<std::quoted(block.name)<<' '<<block.x<<' '<<block.y<<' '<<block.period<<' '<<block.phase<<' '<<std::quoted(hex_text(block.code))<<'\n';
        for(const auto& port:block.inputs)out<<"code_input "<<std::quoted(block.id)<<' '<<std::quoted(port.id)<<' '<<std::quoted(port.name)<<' '<<std::quoted(port.unit)<<' '<<unsigned(port.type)<<' '<<port.initial<<'\n';
        for(const auto& port:block.outputs)out<<"code_output "<<std::quoted(block.id)<<' '<<std::quoted(port.id)<<' '<<std::quoted(port.name)<<' '<<std::quoted(port.unit)<<' '<<unsigned(port.type)<<' '<<port.initial<<'\n';
        for(const auto& pin:block.pin_positions)out<<"x-code-pin "<<std::quoted(block.id)<<' '<<std::quoted(pin.port)<<' '<<pin.x<<' '<<pin.y<<'\n';
        for(const auto& primitive:block.icon) {
            out<<"x-code-icon "<<std::quoted(block.id)<<' '<<unsigned(primitive.kind)<<' '<<unsigned(primitive.color)<<' '
               <<primitive.filled<<' '<<std::quoted(hex_text(primitive.text))<<' '<<primitive.points.size();
            for(const auto& point:primitive.points)out<<' '<<point.x<<' '<<point.y;
            out<<'\n';
        }
    }
    for(const auto &appearance:p.object_icons)for(const auto &primitive:appearance.primitives) {
        out<<"x-object-icon "<<std::quoted(appearance.object)<<' '<<unsigned(primitive.kind)<<' '<<unsigned(primitive.color)<<' '
           <<primitive.filled<<' '<<std::quoted(hex_text(primitive.text))<<' '<<primitive.points.size();
        for(const auto &point:primitive.points)out<<' '<<point.x<<' '<<point.y;
        out<<'\n';
    }
    out<<"scope_enabled "<<p.scope_enabled<<'\n';
    out << "scopeview " << p.scope_begin << ' ' << p.scope_end << ' ' << p.cursor_a << ' ' << p.cursor_b << '\n';
    for(const auto& channel:p.scope_points) out << "scope_point " << std::quoted(channel) << '\n';
    for(const auto& channel:p.scope_channels) out << "scope " << std::quoted(channel) << '\n';
    for(const auto& n:p.nodes) out << "node " << std::quoted(n.id) << ' ' << std::quoted(n.name) << ' ' << n.ground << ' ' << n.x << ' ' << n.y << '\n';
    for(const auto& c:p.components) out << "component " << std::quoted(c.id) << ' ' << std::quoted(c.name) << ' '
        << kind_name(c.kind) << ' ' << std::quoted(c.positive) << ' ' << std::quoted(c.negative) << ' '
        << c.value << ' ' << c.initial << ' ' << c.x << ' ' << c.y << ' ' << c.closed << '\n';
    for(const auto& c:p.components)if(c.kind==Kind::ideal_transformer)
        out<<"transformer_secondary "<<std::quoted(c.id)<<' '<<std::quoted(c.secondary_positive)<<' '<<std::quoted(c.secondary_negative)<<'\n';
    for(const auto& c:p.components)if(c.kind==Kind::dc_motor) {
        validate_motor(c);
        out<<"dc_motor "<<std::quoted(c.id)<<' '<<c.motor.torque_constant<<' '
           <<c.motor.back_emf_constant<<' '<<c.motor.inertia<<' '
           <<c.motor.damping<<' '<<c.motor.load_torque<<'\n';
    }
    for(const auto& c:p.components)if(c.kind==Kind::inductor&&(c.parallel_resistance_enabled||std::abs(c.parallel_resistance-1e12)>1e-9))
        out<<"parallel_resistance "<<std::quoted(c.id)<<' '<<c.parallel_resistance_enabled<<' '<<c.parallel_resistance<<'\n';
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
    for(const auto& v:p.view_options){out<<"x-view "<<std::quoted(v.plot)<<' '<<v.y_low<<' '<<v.y_high<<' '<<v.manual_y<<' '<<v.free_cursors<<' '<<v.separate_axes<<' '<<v.grid<<' '<<v.legend<<' '<<v.line_width<<' '<<v.time_span<<' '<<std::quoted(v.cursor_channel_a)<<' '<<std::quoted(v.cursor_channel_b)<<' '<<v.cursor_y_a<<' '<<v.cursor_y_b<<' '<<v.display_columns<<' '<<v.signal_displays.size();for(const auto& binding:v.signal_displays)out<<' '<<std::quoted(binding.first)<<' '<<binding.second;out<<' '<<v.hidden_channels.size();for(const auto& channel:v.hidden_channels)out<<' '<<std::quoted(channel);out<<' '<<v.curve_styles.size();for(const auto& style:v.curve_styles)out<<' '<<std::quoted(style.channel)<<' '<<unsigned(style.line)<<' '<<style.width<<' '<<unsigned(style.marker)<<' '<<style.marker_size;out<<' '<<v.legend_positions.size();for(const auto& pos:v.legend_positions)out<<' '<<pos.display<<' '<<pos.x<<' '<<pos.y;out<<' '<<v.curve_names.size();for(const auto& name:v.curve_names)out<<' '<<std::quoted(name.first)<<' '<<std::quoted(name.second);out<<' '<<v.viewport<<' '<<v.begin<<' '<<v.end<<' '<<v.cursor_a<<' '<<v.cursor_b<<' '<<v.curve_multipliers.size();for(const auto& multiplier:v.curve_multipliers)out<<' '<<std::quoted(multiplier.first)<<' '<<multiplier.second;out<<'\n';}
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
            out<<"public_port "<<std::quoted(port.id)<<' '<<std::quoted(port.name)<<' '<<std::quoted(port.terminal.object)<<' '<<std::quoted(port.terminal.port)<<' '<<unsigned(port.domain)<<' '<<unsigned(port.direction);
            if(port.has_position)out<<' '<<port.x<<' '<<port.y;
            out<<'\n';
        }
        for(const auto& v:d.parameters) {
            for(const auto* value:{&v.id,&v.name,&v.unit,&v.object,&v.field,&v.group})check_text(*value,d.id);
            if(v.default_expression.size()>1024*1024)
                throw Diagnostic("invalid_parameter_expression",v.id,
                                 "Public parameter default expression exceeds 1 MiB");
            out<<"public_parameter "<<std::quoted(v.id)<<' '<<std::quoted(v.name)<<' '<<std::quoted(v.unit)<<' '<<std::quoted(v.object)<<' '<<std::quoted(v.field)<<' '<<v.value<<' '<<std::quoted(v.group)<<' '<<v.has_minimum<<' '<<v.minimum<<' '<<v.has_maximum<<' '<<v.maximum<<' '<<std::quoted(hex_text(v.default_expression))<<'\n';
        }
        if(d.appearance!=DefinitionAppearance{}) {
            check_text(d.appearance.image_png,d.id);
            out<<"x-appearance "<<d.appearance.symbol<<' '<<std::quoted(d.appearance.image_png)<<'\n';
        }
        Project body;static_cast<Schematic&>(body)=d;body.id=d.id;body.name=d.name;
        out<<"body\n";write_project(body,out);out<<"end_definition\n";
    }
    if(!out) throw Diagnostic("write_error",p.id,"Project write failed");
}
}
