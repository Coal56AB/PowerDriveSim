#include "formats/project/project.hpp"
#include "results/csv.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
using namespace pds;
static int checks=0;
static void require(bool yes,const std::string& message) {
    ++checks;
    if(!yes) throw std::runtime_error(message);
}
static void near(double actual,double expected,double tol,const std::string& label) {
    require(std::isfinite(actual) && std::abs(actual-expected)<=tol,
        label+" actual="+std::to_string(actual)+" expected="+std::to_string(expected));
}
static std::string id(unsigned n) {
    std::ostringstream s; s<<"00000000-0000-4000-8000-"<<std::hex<<std::setfill('0')<<std::setw(12)<<n;
    return s.str();
}
static Project base() {
    Project p; p.id=id(1); p.name="Test";
    p.nodes={{id(2),"ground",true},{id(3),"supply",false},{id(4),"output",false}};
    return p;
}
static Component part(unsigned n,Kind kind,unsigned positive,unsigned negative,double value,double initial=0) {
    return {id(n),"part"+std::to_string(n),kind,id(positive),id(negative),value,initial,0,0,false};
}
static Project rc() {
    auto p=base(); p.profile={0.005,0.000001};
    p.components={part(10,Kind::voltage,3,2,1),part(11,Kind::resistor,3,4,1000),part(12,Kind::capacitor,4,2,1e-6)};
    return p;
}
static Project rlc() {
    auto p=base(); p.nodes.push_back({id(5),"inductor_input",false}); p.profile={0.04,0.000002};
    p.components={part(10,Kind::voltage,3,2,1),part(11,Kind::resistor,3,5,2),
        part(12,Kind::inductor,5,4,0.01),part(13,Kind::capacitor,4,2,0.001)};
    return p;
}
static Project switching() {
    auto p=base(); p.nodes.push_back({id(5),"supply2",false}); p.profile={0.01,0.001};
    p.components={part(10,Kind::voltage,3,2,5),part(11,Kind::voltage,5,2,10),
        part(12,Kind::ideal_switch,3,4,0),part(13,Kind::ideal_switch,5,4,0),part(14,Kind::resistor,4,2,10)};
    p.components[2].closed=true;
    p.events={{0.0043,id(12),false},{0.0043,id(13),true},{0.0081,id(13),false}};
    return p;
}
static size_t channel(const Result& r,unsigned n) {
    for(size_t i=0;i<r.channels.size();++i) if(r.channels[i].object==id(n)) return i;
    throw std::runtime_error("Missing result channel");
}
static double value(const Result& r,const Sample& s,unsigned n) { return s.values[channel(r,n)]; }
static std::string saved(const Project& p) { std::ostringstream out; write_project(p,out); return out.str(); }
static void error(const std::string& code,const std::function<void()>& f) {
    try { f(); } catch(const Diagnostic& d) {
        require(d.code==code,"Expected "+code+" got "+d.code);
        return;
    }
    require(false,"Expected diagnostic "+code);
}
static void unit() {
    require(valid_uuid(id(1)),"UUID");
    require(!valid_uuid("display-name"),"Invalid UUID");
    require(!valid_uuid("00000000-0000-4000-8000-00000000000A"),"Canonical UUID");
    for(auto k:{Kind::resistor,Kind::capacitor,Kind::inductor,Kind::voltage,Kind::current,Kind::ideal_switch})
        require(parse_kind(kind_name(k))==k,"Component type round trip");
    error("unknown_component",[]{parse_kind("Controller");});
    auto p=base(); p.nodes.pop_back();
    p.components={part(10,Kind::current,2,3,2),part(11,Kind::resistor,3,2,4)};
    auto r=execute(compile(p)); near(value(r,r.samples.back(),3),8,1e-12,"Current source polarity / Ohm");
    auto ir=compile(rc()); require(!ir.sparsity.empty(),"Sparse pattern");
    StampSystem a(2); a.add(0,0,2); a.add(0,1,1); a.add(1,0,1); a.add(1,1,3);
    a.inject(0,4); a.inject(1,7);
    auto x=a.solve(ir,0); near(x[0],1,1e-12,"Sparse solve x"); near(x[1],2,1e-12,"Sparse solve y");
    std::atomic_bool stop{true}; r=execute(compile(rc()),&stop);
    require(r.cancelled && r.accepted_steps==0 && r.samples.size()==1,"Cancellation leaves valid prefix");
    std::ostringstream csv; write_csv(r,csv);
    require(csv.str().find("backend=Reference CPU")!=std::string::npos,"Metadata");
    require(csv.str().find("[V]")!=std::string::npos,"CSV units");
}
static double rc_error(Project p) {
    auto r=execute(compile(p)); double max_error=0;
    for(const auto& s:r.samples) max_error=std::max(max_error,std::abs(value(r,s,4)-(1-std::exp(-s.time/.001))));
    return max_error;
}
static void numerical() {
    auto p=rc(); const double e=rc_error(p);
    require(e<0.0002,"RC analytic tolerance 0.2 mV");
    p.profile.step=0.000002; const double coarse=rc_error(p);
    require(coarse/e>1.95 && coarse/e<2.05,"Backward Euler first-order convergence");
    auto r=execute(compile(rlc())); double max_error=0;
    for(const auto& s:r.samples) {
        const double expected=1-std::exp(-100*s.time)*(std::cos(300*s.time)+std::sin(300*s.time)/3);
        max_error=std::max(max_error,std::abs(value(r,s,4)-expected));
    }
    require(max_error<0.001,"RLC analytic tolerance 1 mV");
    std::cout<<"RC max_abs_error="<<e<<" V; RLC max_abs_error="<<max_error<<" V\n";
    p=rc(); p.components[0].value=0; p.components[2].initial=2;
    r=execute(compile(p));
    for(const auto& s:r.samples) near(value(r,s,4),2*std::exp(-s.time/.001),0.0004,"RC initial condition discharge");
    p=rlc(); p.components[0].value=0; p.components[3].initial=1;
    r=execute(compile(p)); double previous=0.0005;
    for(const auto& s:r.samples) {
        double v=value(r,s,4),i=value(r,s,12),energy=.5*.001*v*v+.5*.01*i*i;
        require(energy<=previous+1e-14,"Passive RLC energy cannot increase"); previous=energy;
    }
    require(r.max_scaled_residual<1e-12,"KCL/KVL residual");
}
static void trapezoidal_tests() {
    auto p=rc(); p.profile.method=Method::trapezoidal; p.profile.step=0.00001;
    const double e=rc_error(p); p.profile.step*=2;
    const double coarse=rc_error(p);
    require(e<0.000004,"Trapezoidal RC 4 uV tolerance");
    require(coarse/e>3.95 && coarse/e<4.05,"Trapezoidal second-order convergence");
    p=rlc(); p.profile.method=Method::trapezoidal; p.profile.step=0.00002;
    auto r=execute(compile(p)); double max_error=0;
    for(const auto& sample:r.samples) {
        double t=sample.time;
        double expected=1-std::exp(-100*t)*(std::cos(300*t)+std::sin(300*t)/3);
        max_error=std::max(max_error,std::abs(value(r,sample,4)-expected));
    }
    require(max_error<0.00001,"Trapezoidal RLC 10 uV tolerance");
    std::cout<<"Trapezoidal RC max_abs_error="<<e<<" V; RLC max_abs_error="<<max_error<<" V\n";
    // Lossless LC, no source: trapezoidal preserves quadratic stored energy.
    p=base(); p.nodes.pop_back(); p.profile={0.1,0.001,Method::trapezoidal};
    p.components={part(10,Kind::capacitor,3,2,.001,1),part(11,Kind::inductor,3,2,.01)};
    r=execute(compile(p));
    for(const auto& s:r.samples) {
        double v=value(r,s,3),i=value(r,s,11);
        near(.5*.001*v*v+.5*.01*i*i,.0005,1e-14,"Trapezoidal lossless LC energy");
    }
    // A gate edge changes the RC driving voltage; the post-event capacitor
    // current must initialize the next trapezoidal interval.
    p=switching(); p.events.resize(2);
    p.profile={.01,.00001,Method::trapezoidal};
    p.nodes.push_back({id(6),"capacitor",false});
    p.components.push_back(part(20,Kind::resistor,4,6,1000));
    p.components.push_back(part(21,Kind::capacitor,6,2,1e-6));
    r=execute(compile(p));
    const double at_edge=5*(1-std::exp(-.0043/.001));
    for(const auto& s:r.samples) {
        double expected=s.time<=.0043?5*(1-std::exp(-s.time/.001)):10+(at_edge-10)*std::exp(-(s.time-.0043)/.001);
        near(value(r,s,6),expected,0.00002,"Gate edge C continuity and derivative restart");
    }
}
static void serialization() {
    auto p=switching(); p.name="Unicode схема / quoted \"name\""; p.components[0].x=123.5;
    p.extensions={"x-test future_extension {keep this exactly}"};
    auto text=saved(p); std::istringstream in(text); auto q=read_project(in);
    require(saved(q)==text,"Lossless save/load including UUID, geometry, events, unknown extension");
    auto a=execute(compile(p)),b=execute(compile(q));
    require(a.samples.back().values==b.samples.back().values,"Semantics round trip");
    error("schema_version",[]{std::istringstream s("PowerDriveSim 99\n"); read_project(s);});
    error("parse_error",[]{std::istringstream s(""); read_project(s);});
    error("parse_error",[]{std::istringstream s("PowerDriveSim\n"); read_project(s);});
    error("parse_error",[]{std::istringstream s("PowerDriveSim 1\nunknown 5\n"); read_project(s);});
    error("parse_error",[&]{std::istringstream s(text+"profile 1 0.1\n"); read_project(s);});
    error("parse_error",[&]{std::istringstream s(text+"event 0.01 \"id\" 2\n"); read_project(s);});
    p.profile.method=Method::trapezoidal;
    auto method_text=saved(p); std::istringstream method_in(method_text);
    require(read_project(method_in).profile.method==Method::trapezoidal,"Method round trip");

    // CTest uses build as cwd, so migration also has a self-contained fixture.
    std::string old=text; old.replace(0,15,"PowerDriveSim 1");
    const auto method_at=old.find(" BackwardEuler");
    require(method_at!=std::string::npos,"Fixture contains v2 method");
    old.erase(method_at,14);
    std::istringstream old_in(old); auto migrated=read_project(old_in);
    require(migrated.schema==2 && migrated.profile.method==Method::backward_euler,"Explicit v1 migration");
    require(migrated.extensions==p.extensions,"Migration preserves unknown extensions");
    error("invalid_method",[]{parse_method("magic");});
    p.extensions={"x-invalid\nnode"};
    error("extension_error",[&]{saved(p);});
}
static void topology() {
    auto p=rc(); p.nodes[0].ground=false;
    error("missing_ground",[&]{compile(p);});
    p=rc(); p.nodes.push_back({id(20),"floating",false});
    error("floating_node",[&]{compile(p);});
    p=rc(); p.components[1].positive=id(999);
    error("missing_terminal",[&]{compile(p);});
    p=rc(); p.components[1].value=0;
    error("invalid_parameter",[&]{compile(p);});
    p=rc(); p.profile.step=std::numeric_limits<double>::infinity();
    error("invalid_profile",[&]{compile(p);});
    p=rc(); p.components[1].id=p.components[0].id;
    error("invalid_uuid",[&]{compile(p);});
    p=rc(); p.components.push_back(part(20,Kind::voltage,3,2,2));
    error("singular_matrix",[&]{execute(compile(p));});
    p=rc(); p.events={{0.001,id(11),true}};
    error("invalid_gate_target",[&]{compile(p);});
    p=switching(); p.events.push_back(p.events.front());
    error("conflicting_gate_events",[&]{compile(p);});
    p=switching(); p.events[0].time=-1;
    error("invalid_event",[&]{compile(p);});
    p=switching(); p.events[0].time=std::numeric_limits<double>::quiet_NaN();
    error("invalid_event",[&]{compile(p);});
    p=rc(); p.profile.method=static_cast<Method>(99);
    error("invalid_method",[&]{compile(p);});
    p=base(); p.nodes.pop_back(); p.components={part(10,Kind::current,2,3,1)};
    error("floating_node",[&]{compile(p);});
    p=base(); p.nodes.pop_back(); p.components={part(10,Kind::ideal_switch,2,3,0)};
    error("singular_matrix",[&]{execute(compile(p));});
}
static void regression() {
    auto p=switching(); auto ir=compile(p); auto a=execute(ir),b=execute(ir);
    require(a.samples.size()==b.samples.size(),"Repeat sample count");
    bool edge1=false,edge2=false;
    for(size_t i=0;i<a.samples.size();++i) {
        const auto& s=a.samples[i];
        require(s.time==b.samples[i].time && s.values==b.samples[i].values && s.gates==b.samples[i].gates,"Bitwise repeatability");
        near(value(a,s,4),s.time<.0043?5:(s.time<.0081?10:0),1e-12,"Exact ideal switching");
        if(s.time==.0043) edge1=true;
        if(s.time==.0081) edge2=true;
        if(i) require(s.time>a.samples[i-1].time,"Strictly increasing samples");
    }
    require(edge1 && edge2,"Non-grid events sampled exactly");
    std::reverse(p.components.begin(),p.components.end()); std::reverse(p.nodes.begin(),p.nodes.end());
    std::reverse(p.events.begin(),p.events.end()); b=execute(compile(p));
    require(a.samples.size()==b.samples.size(),"Permutation samples");
    for(size_t i=0;i<a.samples.size();++i) require(a.samples[i].values==b.samples[i].values,"Order-independent atomic events");
    near(a.samples.back().time,.01,0,"Exact stop time");
    p=switching(); p.events={{0,id(12),false},{0,id(13),true},{.01,id(13),false}};
    a=execute(compile(p)); near(value(a,a.samples.front(),4),10,1e-12,"Time zero events");
    near(value(a,a.samples.back(),4),0,1e-12,"Stop-time events");
}
static void examples(const std::string& root) {
    for(const auto& name:{"rc","rlc","switch","rc-trapezoidal"}) {
        std::ifstream in(root+"/examples/"+name+".pds"); auto p=read_project(in); auto r=execute(compile(p));
        require(!r.samples.empty() && r.samples.back().time==p.profile.stop,"Example completes");
        if(std::string(name)=="rc" || std::string(name)=="rc-trapezoidal") near(value(r,r.samples.back(),4),1-std::exp(-5),0.0002,"RC example");
        if(std::string(name)=="rlc") {
            double t=p.profile.stop;
            near(value(r,r.samples.back(),4),1-std::exp(-100*t)*(std::cos(300*t)+std::sin(300*t)/3),0.001,"RLC example");
        }
        if(std::string(name)=="switch") near(value(r,r.samples.back(),4),0,1e-12,"Switch example");
    }
}
int main(int argc,char** argv) {
    try {
        std::string group=argc>1?argv[1]:"all",root=argc>2?argv[2]:".";
        if(group=="unit" || group=="all") unit();
        if(group=="numerical" || group=="all") { numerical(); trapezoidal_tests(); }
        if(group=="serialization" || group=="all") serialization();
        if(group=="topology" || group=="all") topology();
        if(group=="regression" || group=="all") regression();
        if(group=="examples" || group=="all") examples(root);
        require(checks>0,"Unknown test group");
        std::cout<<"PASS "<<group<<" checks="<<checks<<'\n'; return 0;
    } catch(const Diagnostic& d) { std::cerr<<d.code<<" object="<<d.object<<" time="<<d.time<<" "<<d.what()<<'\n'; return 1;
    } catch(const std::exception& e) { std::cerr<<"FAIL "<<e.what()<<'\n'; return 1; }
}