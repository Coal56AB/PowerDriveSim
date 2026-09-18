#include "core/editor/document.hpp"
#include "core/editor/properties.hpp"
#include "core/model/hierarchy.hpp"
#include "formats/project/project.hpp"
#include "core/solver/reference/reference.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <sstream>
#include <fstream>
using namespace pds;
static void check(bool yes,const char* label) { if(!yes) throw std::runtime_error(label); }
static std::string save(const Project& p) { std::ostringstream out; write_project(p,out); return out.str(); }
template<class F> static void error(const std::string& code,F fn) {
    try { fn(); } catch(const Diagnostic& d) { check(d.code==code,"Unexpected diagnostic"); return; }
    throw std::runtime_error("Missing diagnostic: "+code);
}
static double output(const Result& r,const std::string& id) {
    for(size_t i=0;i<r.channels.size();++i) if(r.channels[i].object==id) return r.samples.back().values[i];
    throw std::runtime_error("Missing channel");
}
int main(int argc,char** argv) {
    try {
        Project p; p.id=new_uuid(); p.name="New circuit"; p.wired=true; p.profile={.005,1e-6};
        {
            Project references; references.id=new_uuid(); references.wired=true;
            references.nodes={{new_uuid(),"GND1",true},{new_uuid(),"GND1",true},{new_uuid(),"GND2",true}};
            const auto resolved=resolve_connections(references);
            const auto first=resolved.nets.at(endpoint_key({references.nodes[0].id,"node"}));
            const auto repeated=resolved.nets.at(endpoint_key({references.nodes[1].id,"node"}));
            const auto isolated=resolved.nets.at(endpoint_key({references.nodes[2].id,"node"}));
            check(first==repeated&&first!=isolated,"Ground names define shared and isolated reference nets");
        }
        {
            Project differential; differential.id=new_uuid(); differential.wired=true;
            Document view(differential);
            const auto positive=view.add_component(Kind::voltage_probe,0,0);
            const auto negative=view.add_component(Kind::voltage_probe,0,100);
            const auto graph=view.add_plot(300,50,"Differential",true);
            view.connect({positive,"out"},{graph,"p1"});
            view.connect({negative,"out"},{graph,"n1"});
            const auto sources=plot_source_channels(view.project(),graph);
            const auto pairs=plot_differential_channels(view.project(),graph);
            const auto displayed=plot_channels(view.project(),graph);
            check(sources==std::vector<std::string>{positive,negative}&&pairs.size()==2&&
                      pairs[0]==std::pair<std::string,std::string>{positive,negative}&&
                      displayed==std::vector<std::string>{"diff/"+graph+"/1"},
                  "Differential plot records operands and exposes their difference");
            std::istringstream input(save(view.project()));
            const auto restored=read_project(input);
            check(restored.schema==project_schema&&restored.plots.front().differential,
                  "Differential plot roundtrip");
        }
        {
            Project junctions; junctions.id=new_uuid(); junctions.wired=true;
            Document drawing(junctions);
            const auto left=drawing.add_node(false,0,0), right=drawing.add_node(false,100,0);
            const auto branch=drawing.add_node(false,50,100);
            drawing.connect({left,"node"},{right,"node"});
            const auto trunk=drawing.project().wires.front().id;
            WireAnchor target; target.wire=trunk; target.point={50,0}; target.route={{0,0},{100,0}};
            drawing.connect_anchors({{branch,"node"}},target,{});
            check(drawing.project().nodes.size()==4&&drawing.project().wires.size()==3,
                  "Branch creates one explicit junction and splits the trunk once");
            const auto& joint=drawing.project().nodes.back();
            check(joint.x==50&&joint.y==0&&
                      std::count_if(drawing.project().wires.begin(),drawing.project().wires.end(),[&](const Wire& wire){
                          return wire.from.object==joint.id||wire.to.object==joint.id;
                      })==3,
                  "Branch terminates exactly at a T-junction without a shared visual tail");
        }
        {
            Project cleanup; cleanup.id=new_uuid(); cleanup.wired=true;
            Document drawing(cleanup);
            const auto junction=drawing.add_node(false,0,0);
            const auto resistor=drawing.add_component(Kind::resistor,100,0);
            drawing.connect({junction,"node"},{resistor,"p"});
            drawing.erase({resistor});
            check(drawing.project().nodes.empty(),"Deleting the last connected object removes its orphan junction");
            drawing.undo();
            check(drawing.project().nodes.size()==1&&drawing.project().components.size()==1,
                  "Orphan cleanup is part of the same undoable deletion");
        }
        {
            constexpr const char *source_definition = "1a963f2c-ceb8-5cce-b927-44d735ec9e80";
            constexpr const char *voltage_parameter = "6c1aaf47-7a4f-5dac-ab25-cdd71f6816b3";
            constexpr const char *kind_parameter = "9e07a7ea-8295-5fd0-92c6-6e82c8f1c21b";
            Project legacy; legacy.id=new_uuid(); legacy.wired=true;
            Definition body; body.id=source_definition; body.name="Three-phase voltage source Y";
            body.parameters.push_back({voltage_parameter,"U phase peak","V","*","value",310});
            legacy.definitions.push_back(body);
            legacy.instances.push_back({new_uuid(),"Three-phase voltage source Y",source_definition,0,0});
            check(std::get<unsigned>(read_property(legacy,legacy.instances.front().id,"three_phase_voltage_kind"))==2,
                  "Legacy three-phase source defaults to phase peak");
            check(std::abs(std::get<double>(read_property(legacy,legacy.instances.front().id,"three_phase_voltage"))-310)<1e-12,
                  "Legacy three-phase source voltage remains readable");
            write_property(legacy,legacy.instances.front().id,"three_phase_voltage_kind",unsigned(1));
            check(legacy.instances.front().parameters.front().first==kind_parameter,
                  "Legacy three-phase source stores voltage kind when edited");
        }
        Document doc(p);
        doc.set_view("",.001,.005,.002,.004);
        check(!doc.can_undo(),"Viewing results does not create model history");
        auto visual=doc.project();visual.scope_begin=.003;check(same_simulation(visual,doc.project()),"View does not invalidate numerical results");
        visual.scope_enabled=true;visual.scope_channels={"view-only"};check(same_simulation(visual,doc.project()),"Recording preferences do not invalidate existing results");
        doc.apply("No change",[](Project&){});check(!doc.can_undo(),"Empty transaction does not consume undo history");
        const auto g=doc.add_node(true,0,240);
        const auto v=doc.add_component(Kind::voltage,0,0);
        const auto r=doc.add_component(Kind::resistor,220,0);
        const auto c=doc.add_component(Kind::capacitor,440,0);
        const auto probe=doc.add_component(Kind::voltage_probe,440,140);
        doc.connect({v,"n"},{g,"node"}); doc.connect({v,"p"},{r,"p"});
        doc.connect({r,"n"},{c,"p"}); doc.connect({c,"n"},{g,"node"});
        doc.connect({probe,"p"},{c,"p"}); doc.connect({probe,"n"},{g,"node"});
        auto result=execute(compile(doc.project()));
        check(std::abs(output(result,probe)-(1-std::exp(-5)))<.0002,"Visual RC analytical result");
        doc.apply("Name passive elements",[&](Project& project){
            for(auto& component:project.components) {
                if(component.id==r)component.name="R1";
                if(component.id==c)component.name="C1";
            }
        });
        auto named=doc.copy({r,c});
        auto named_ids=doc.paste(named,80,80);
        check(named_ids.size()==2,"Group copy keeps selected passive elements");
        auto names=std::vector<std::string>{};
        for(const auto& component:doc.project().components)names.push_back(component.name);
        check(std::find(names.begin(),names.end(),"R2")!=names.end()&&std::find(names.begin(),names.end(),"C2")!=names.end(),
              "Copy auto-increments R1/C1 names without nested suffixes");
        doc.undo(); // paste
        doc.undo(); // rename
        const auto connected=save(doc.project());
        const auto wire=doc.project().wires[1].id;
        doc.erase({wire});
        auto resolved=resolve_connections(doc.project());
        check(resolved.nets.at(endpoint_key({v,"p"}))!=resolved.nets.at(endpoint_key({r,"p"})),"Deleting wire splits net");
        doc.undo(); check(save(doc.project())==connected,"Undo restores entire document");
        doc.redo(); check(doc.project().wires.size()==5,"Redo deletion");
        doc.undo();
        error("duplicate_connection",[&]{doc.connect({r,"p"},{v,"p"});});
        check(save(doc.project())==connected,"Failed transaction does not change history or model");
        auto pattern=doc.add_pattern(200,400),sw=doc.add_component(Kind::ideal_switch,200,500);
        error("incompatible_port",[&]{doc.connect({pattern,"out"},{r,"p"});});
        error("incompatible_port",[&]{doc.connect({probe,"out"},{sw,"gate"});});
        doc.connect({pattern,"out"},{sw,"gate"});
        doc.apply("Pattern edges",[&](Project& project){project.events={{.001,pattern,true},{.003,pattern,false}};});
        resolved=resolve_connections(doc.project());
        check(resolved.project.events.size()==2 && resolved.project.events[0].target==sw,"Pattern becomes gate events");
        auto pattern2=doc.add_pattern(400,400);
        error("multiple_gate_drivers",[&]{doc.connect({pattern2,"out"},{sw,"gate"});});
        const auto old_driver=save(doc.project());
        doc.connect_anchors({{pattern2,"out"}},{{sw,"gate"}},{},"",true);
        check(doc.project().events.size()==2,"Replacing driver preserves the old source's own events");
        check(resolve_connections(doc.project()).project.events.empty(),"Only the connected source drives the switch");
        doc.undo();check(save(doc.project())==old_driver,"Driver replacement is one undoable transaction");
        doc.erase({pattern2,sw,pattern});
        check(doc.project().events.empty(),"Deletion removes dependent events and wires");
        doc.apply("Route, style and scope",[&](Project& project){
            project.wires[0].bends={{-120,100},{-120,240}};
            project.wires[0].color="#c04080";
            project.wires[0].width=3.5;
            project.wires[0].line=WireLine::dash;
            project.scope_channels={probe};
        });
        check(std::get<std::string>(read_property(doc.project(),doc.project().wires[0].id,"wire_color"))=="#c04080" &&
                  std::abs(std::get<double>(read_property(doc.project(),doc.project().wires[0].id,"wire_width"))-3.5)<1e-12 &&
                  std::get<unsigned>(read_property(doc.project(),doc.project().wires[0].id,"wire_line"))==unsigned(WireLine::dash),
              "Wire visual properties are editable");
        const auto encoded=save(doc.project());
        std::istringstream input(encoded); auto roundtrip=read_project(input);
        check(save(roundtrip)==encoded,"Wire geometry, scope, UUID and model round trip");
        check(execute(compile(roundtrip)).samples.back().values==execute(compile(doc.project())).samples.back().values,"Saved circuit semantics");
        doc.apply("Scale visual element",[&](Project& project){project.components[1].orientation.scale=1.5;});
        const auto scaled=save(doc.project());std::istringstream scaled_input(scaled);auto scaled_roundtrip=read_project(scaled_input);
        check(std::abs(scaled_roundtrip.components[1].orientation.scale-1.5)<1e-12,"Element visual scale roundtrip");
        check(same_simulation(scaled_roundtrip,roundtrip),"Element scale is visual-only");
        doc.undo();check(save(doc.project())==encoded,"Scale undo");
        doc.apply("Move",[&](Project& project){project.components.front().x+=100;});
        doc.undo(); check(save(doc.project())==encoded,"Move undo"); doc.redo();
        check(doc.project().components.front().x==100,"Move redo");
        check(std::abs(parse_si("1 kOhm","Ohm")-1000)<1e-12,"Resistance units");
        check(std::abs(parse_si("10 µF","F")-1e-5)<1e-15,"Microfarads");
        check(std::abs(parse_si("2.5e-3 H","H")-.0025)<1e-15,"Engineering notation");
        error("incompatible_unit",[]{parse_si("3 A","V");});
        error("invalid_parameter",[]{parse_si("nan","V");});
        if(argc>1) {
            std::ifstream old(std::string(argv[1])+"/examples/rc.pds"); auto legacy=read_project(old);
            const auto converted=make_wired(legacy); auto a=execute(compile(legacy)),b=execute(compile(converted));
            check(a.samples.back().values==b.samples.back().values,"Migrated visual RC preserves solution");
            check(converted.wires.size()==6,"Legacy net expands to explicit wires");
        }
        // An ideal current probe inserts a zero-voltage branch.
        p=Project{}; p.id=new_uuid(); p.wired=true; Document measured(p);
        auto source=measured.add_component(Kind::voltage,0,0);
        auto resistor=measured.add_component(Kind::resistor,200,0);
        auto current=measured.add_component(Kind::current_probe,400,0);
        auto ground=measured.add_node(true,0,200);
        measured.connect({source,"p"},{resistor,"p"}); measured.connect({resistor,"n"},{current,"p"});
        measured.connect({current,"n"},{ground,"node"}); measured.connect({source,"n"},{ground,"node"});
        check(std::abs(output(execute(compile(measured.project())),current)-.001)<1e-12,"Current probe polarity");
        // Removing a degree-two node preserves electrical continuity and is one undo step.
        auto joint=measured.add_node(false,100,0);
        auto direct=measured.project().wires.front().id;
        measured.erase({direct});
        measured.connect({joint,"node"},{source,"p"});
        measured.connect({resistor,"p"},{joint,"node"});
        measured.apply("Observe net",[&](Project& p){p.scope_channels={resolve_connections(p).nets.at(endpoint_key({source,"p"}))};});
        const auto before_joint=save(measured.project());
        const auto before_current=output(execute(compile(measured.project())),current);
        measured.remove_junction(joint,{{100,0},{100,40},{0,40},{0,0}},{{200,0},{100,0}});
        check(measured.project().nodes.size()==1,"Only pass-through node removed");
        check(measured.project().wires.size()==4,"Two adjacent wires merged");
        check(measured.project().scope_channels.front()==resolve_connections(measured.project()).nets.at(endpoint_key({source,"p"})),"Scope subscription follows merged net");
        check(output(execute(compile(measured.project())),current)==before_current,"Removing point preserves physical result");
        const auto after_joint=save(measured.project());
        measured.undo();check(save(measured.project())==before_joint,"Junction undo restores full geometry");
        measured.redo();check(save(measured.project())==after_joint,"Junction redo restores merged wire");
        // Connection tags join distant endpoints by name without long visual wires.
        p=Project{}; p.id=new_uuid(); p.wired=true; Document tagged(p);
        auto tagged_v=tagged.add_component(Kind::voltage,0,0);
        auto tagged_r=tagged.add_component(Kind::resistor,300,0);
        auto tagged_g=tagged.add_node(true,0,160);
        const auto tag_a=derived_uuid("tag-a"),tag_b=derived_uuid("tag-b");
        tagged.apply("Add electrical tags",[&](Project& project){
            project.tags.push_back({tag_a,"DC+",120,-80,Domain::electrical});
            project.tags.push_back({tag_b,"DC+",220,-80,Domain::electrical});
        });
        tagged.connect({tagged_v,"n"},{tagged_g,"node"});
        tagged.connect({tagged_r,"n"},{tagged_g,"node"});
        tagged.connect({tagged_v,"p"},{tag_a,"io"});
        tagged.connect({tag_b,"io"},{tagged_r,"p"});
        auto tagged_graph=resolve_connections(tagged.project());
        check(tagged_graph.nets.at(endpoint_key({tagged_v,"p"}))==tagged_graph.nets.at(endpoint_key({tagged_r,"p"})),
              "Electrical tags join equally named endpoints");
        auto tagged_copy=tagged.copy({tag_a,tag_b});
        auto tagged_pasted=tagged.paste(tagged_copy,40,40);
        check(tagged_pasted.size()==2&&tagged.project().tags[2].name=="DC+"&&tagged.project().tags[3].name=="DC+",
              "Copy keeps tag names for wireless groups");
        std::istringstream tagged_reload(save(tagged.project()));
        auto tagged_roundtrip=read_project(tagged_reload);
        check(tagged_roundtrip.tags.size()==4&&tagged_roundtrip.tags[0].name=="DC+",
              "Connection tags roundtrip");
        auto tag_visual=tagged.project();
        tag_visual.tags[0].x+=80;
        check(same_simulation(tag_visual,tagged.project()),"Moving a tag is visual-only");
        auto tag_renamed=tagged.project();
        tag_renamed.tags[0].name="DC_ALT";
        check(!same_simulation(tag_renamed,tagged.project()),"Tag names affect connectivity");
        // Scope rules use hierarchy paths, not display names. Local tags stay
        // inside an instance, ancestor tags reach an explicit parent tag, and
        // global tags span independent instances.
        Project scoped;scoped.id=new_uuid();scoped.wired=true;
        Definition scoped_child;scoped_child.id=derived_uuid("scoped-definition");scoped_child.name="Scoped";scoped_child.wired=true;
        const auto local_tag=derived_uuid("scoped-local"),up_tag=derived_uuid("scoped-up"),global_tag=derived_uuid("scoped-global");
        scoped_child.tags.push_back({local_tag,"LOCAL",0,0,Domain::electrical});
        scoped_child.tags.push_back({up_tag,"UP",0,20,Domain::electrical,{},TagScope::ancestors});
        scoped_child.tags.push_back({global_tag,"GLOBAL",0,40,Domain::electrical,{},TagScope::global,false});
        scoped.definitions.push_back(scoped_child);
        const auto instance_a=derived_uuid("scoped-a"),instance_b=derived_uuid("scoped-b");
        scoped.instances.push_back({instance_a,"A",scoped_child.id,0,0});
        scoped.instances.push_back({instance_b,"B",scoped_child.id,200,0});
        const auto root_local=derived_uuid("root-local"),root_up=derived_uuid("root-up"),root_global=derived_uuid("root-global");
        scoped.tags.push_back({root_local,"LOCAL",0,100,Domain::electrical});
        scoped.tags.push_back({root_up,"UP",20,100,Domain::electrical});
        scoped.tags.push_back({root_global,"GLOBAL",40,100,Domain::electrical});
        auto scoped_flat=flatten(scoped).project;
        auto scoped_graph=resolve_connections(scoped_flat);
        auto net=[&](const std::string& object){return scoped_graph.nets.at(endpoint_key({object,"io"}));};
        const auto a_local=expanded_uuid({instance_a},local_tag),b_local=expanded_uuid({instance_b},local_tag);
        const auto a_up=expanded_uuid({instance_a},up_tag),b_up=expanded_uuid({instance_b},up_tag);
        const auto a_global=expanded_uuid({instance_a},global_tag),b_global=expanded_uuid({instance_b},global_tag);
        check(net(a_local)!=net(root_local)&&net(a_local)!=net(b_local),"Local tag scope isolates instances");
        check(net(a_up)==net(root_up)&&net(b_up)==net(root_up),"Ancestor tag scope reaches the parent bus");
        check(net(a_global)==net(root_global)&&net(b_global)==net(root_global),"Global tag scope spans instances");
        std::istringstream scoped_input(save(scoped));auto scoped_roundtrip=read_project(scoped_input);
        check(scoped_roundtrip.definitions.front().tags.back().scope==TagScope::global&&
              !scoped_roundtrip.definitions.front().tags.back().listed,"Tag scope and list visibility roundtrip");
        auto listed_only=scoped;listed_only.definitions.front().tags.back().listed=true;
        check(same_simulation(scoped,listed_only),"Tag list visibility is visual-only");
        Document scoped_document(scoped);
        const auto scoped_before=save(scoped_document.root_project());
        scoped_document.apply("Edit tag scope",[&](Project& project){
            project.tags[1].scope=TagScope::global;
            project.tags[1].name="UP_GLOBAL";
            project.tags[1].listed=false;
        });
        const auto scoped_after=save(scoped_document.root_project());
        check(scoped_after!=scoped_before,"Tag edits create a document transaction");
        scoped_document.undo();check(save(scoped_document.root_project())==scoped_before,"Tag edit undo restores scope, name and listing");
        scoped_document.redo();check(save(scoped_document.root_project())==scoped_after,"Tag edit redo restores scope, name and listing");
        scoped_document.erase({root_local});
        check(scoped_document.root_project().tags.size()==2,"Tag deletion removes the selected tag");
        scoped_document.undo();check(scoped_document.root_project().tags.size()==3,"Tag deletion undo restores the tag");
        Project observed;observed.id=new_uuid();observed.wired=true;Document observed_document(observed);
        const auto observed_probe=observed_document.add_component(Kind::voltage_probe,0,0);
        const auto observed_plot=observed_document.add_plot(240,0);
        const auto signal_tx=derived_uuid("signal-tag-tx"),signal_rx=derived_uuid("signal-tag-rx");
        observed_document.apply("Add signal tags",[&](Project& project){
            project.tags.push_back({signal_tx,"MEASURE",80,0,Domain::signal});
            project.tags.push_back({signal_rx,"MEASURE",160,0,Domain::signal});
        });
        observed_document.connect({observed_probe,"out"},{signal_tx,"io"});
        observed_document.connect({signal_rx,"io"},{observed_plot,"in1"});
        check(plot_channels(observed_document.project(),observed_plot)==std::vector<std::string>{observed_probe},
              "Signal tags expose their real source channel to a graph");
        Project observed_gate;observed_gate.id=new_uuid();observed_gate.wired=true;Document gate_plot_document(observed_gate);
        const auto observed_pattern=gate_plot_document.add_pattern(0,0);
        const auto gate_plot=gate_plot_document.add_plot(240,0);
        const auto gate_plot_tx=derived_uuid("gate-plot-tag-tx"),gate_plot_rx=derived_uuid("gate-plot-tag-rx");
        gate_plot_document.apply("Add observed gate tags",[&](Project& project){
            project.tags.push_back({gate_plot_tx,"OBS_GATE",80,0,Domain::gate});
            project.tags.push_back({gate_plot_rx,"OBS_GATE",160,0,Domain::gate});
        });
        gate_plot_document.connect({observed_pattern,"out"},{gate_plot_tx,"io"});
        gate_plot_document.connect({gate_plot_rx,"io"},{gate_plot,"in1"});
        check(plot_channels(gate_plot_document.project(),gate_plot)==std::vector<std::string>{"gate/"+observed_pattern},
              "Gate tags expose their real source channel to a graph");
        // Gate tags route a programmable gate source to inputs without adding direct pattern-to-switch wires.
        p=Project{}; p.id=new_uuid(); p.wired=true; p.profile={.003,1e-4}; Document gate_tagged(p);
        auto gate_source=gate_tagged.add_pattern(0,0);
        auto gate_supply=gate_tagged.add_component(Kind::voltage,0,120);
        auto gate_switch=gate_tagged.add_component(Kind::ideal_switch,200,120);
        auto gate_load=gate_tagged.add_component(Kind::resistor,400,120);
        auto gate_ground=gate_tagged.add_node(true,0,260);
        const auto gate_tx=derived_uuid("gate-tag-tx"),gate_rx=derived_uuid("gate-tag-rx");
        gate_tagged.apply("Gate tag",[&](Project& project){
            project.patterns.back().pwm=true;project.patterns.back().frequency=1000;project.patterns.back().duty=.5;
            project.tags.push_back({gate_tx,"Q1",80,0,Domain::gate});
            project.tags.push_back({gate_rx,"Q1",160,0,Domain::gate});
        });
        gate_tagged.connect({gate_supply,"p"},{gate_switch,"p"});
        gate_tagged.connect({gate_switch,"n"},{gate_load,"p"});
        gate_tagged.connect({gate_load,"n"},{gate_ground,"node"});
        gate_tagged.connect({gate_supply,"n"},{gate_ground,"node"});
        gate_tagged.connect({gate_source,"out"},{gate_tx,"io"});
        gate_tagged.connect({gate_rx,"io"},{gate_switch,"gate"});
        auto gate_ir=compile(gate_tagged.project());
        check(std::count_if(gate_ir.events.begin(),gate_ir.events.end(),[&](const GateEvent& e){return e.target==gate_switch;})>=4,
              "Gate tags distribute PWM edges to switch inputs");
        std::cout<<"PASS editor model: wiring, probes, undo, SI, schema4, migration, gates\n";
        return 0;
    } catch(const Diagnostic& d) { std::cerr<<d.code<<" "<<d.object<<": "<<d.what()<<'\n'; return 1;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
