#include "core/editor/document.hpp"
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
        Document doc(p);
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
        doc.erase({pattern2,sw,pattern});
        check(doc.project().events.empty(),"Deletion removes dependent events and wires");
        doc.apply("Route and scope",[&](Project& project){project.wires[0].bends={{-120,100},{-120,240}}; project.scope_channels={probe};});
        const auto encoded=save(doc.project());
        std::istringstream input(encoded); auto roundtrip=read_project(input);
        check(save(roundtrip)==encoded,"Wire geometry, scope, UUID and model round trip");
        check(execute(compile(roundtrip)).samples.back().values==execute(compile(doc.project())).samples.back().values,"Saved circuit semantics");
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
        std::cout<<"PASS editor model: wiring, probes, undo, SI, schema4, migration, gates\n";
        return 0;
    } catch(const Diagnostic& d) { std::cerr<<d.code<<" "<<d.object<<": "<<d.what()<<'\n'; return 1;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
