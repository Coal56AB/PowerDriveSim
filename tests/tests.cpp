#include "formats/project/project.hpp"
#include "results/csv.hpp"
#include "core/solver/reference/factorization.hpp"
#include "core/model/expression.hpp"
#include "core/model/connectivity.hpp"
#include "core/model/c_program.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numbers>
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
static size_t named_channel(const Result &r, unsigned n, const std::string &prefix) {
    for(size_t i=0;i<r.channels.size();++i)
        if((r.channels[i].object==id(n) || r.channels[i].object=="omega/"+id(n)) &&
           r.channels[i].name.starts_with(prefix)) return i;
    throw std::runtime_error("Missing named result channel");
}
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
    for(auto k:{Kind::resistor,Kind::capacitor,Kind::inductor,Kind::voltage,Kind::current,
                Kind::ideal_switch,Kind::ideal_transformer,Kind::dc_motor})
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
    const auto expression=parse_expression_program(
        "const double base = 20; double scaled = base * 2.5; return max(scaled, 10);");
    near(evaluate_expression(expression.expression,expression.variables),50,1e-12,
         "Shared expression evaluator");
    error("invalid_expression",[]{
        const auto cyclic=parse_expression_program("double a = b; double b = a;");
        (void)evaluate_expression("a",cyclic.variables);
    });
    error("invalid_expression",[]{(void)evaluate_expression("t + 1");});
    error("invalid_expression",[]{(void)parse_expression_program("double a=1; double a=2;");});
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

    // Ideal transformer with Np/Ns=2 feeding an 8-ohm resistive load.
    // Its reflected primary resistance is n^2 R = 32 ohms.
    p=base(); p.profile={.04,.0001};
    p.components={part(10,Kind::voltage,3,2,10),part(11,Kind::ideal_transformer,3,2,2),
                  part(12,Kind::resistor,4,2,8)};
    p.components[0].source.kind=Waveform::sine;
    p.components[0].source.frequency=50;
    p.components[1].secondary_positive=id(4);
    p.components[1].secondary_negative=id(2);
    r=execute(compile(p));
    for(const auto& s:r.samples) {
        const double primary=10*std::sin(2*std::numbers::pi*50*s.time);
        const double secondary=primary/2;
        near(value(r,s,3),primary,1e-12,"Transformer primary AC voltage");
        near(value(r,s,4),secondary,1e-12,"Transformer turns ratio");
        near(value(r,s,11),primary/32,1e-12,"Transformer reflected load current");
        const double source_power=primary*value(r,s,10);
        const double load_power=secondary*secondary/8;
        near(source_power+load_power,0,1e-12,"Transformer source/load power conservation");
    }

    // Permanent-magnet brushed DC motor with a rigid inertial shaft. The
    // electrical and mechanical equations reduce to one first-order ODE for
    // constant voltage, providing an independent analytical reference.
    auto motor_project = [&](Method method) {
        auto model=base();
        model.nodes.pop_back();
        model.profile={.25,.0001,method};
        auto motor=part(11,Kind::dc_motor,3,2,2,.4);
        motor.motor={.2,.2,.02,.01,.05};
        model.components={part(10,Kind::voltage,3,2,12),motor};
        return model;
    };
    for(const auto method:{Method::backward_euler,Method::trapezoidal}) {
        p=motor_project(method);
        r=execute(compile(p));
        const auto omega_channel=named_channel(r,11,"omega:");
        const auto current_channel=named_channel(r,11,"i:");
        const auto &motor=p.components[1];
        const auto &m=motor.motor;
        const double decay=(m.torque_constant*m.back_emf_constant/motor.value+m.damping)/m.inertia;
        const double steady=(m.torque_constant*12/motor.value-m.load_torque)/
                            (m.torque_constant*m.back_emf_constant/motor.value+m.damping);
        double electrical_energy=0,copper_energy=0,damping_energy=0,load_energy=0;
        for(size_t k=0;k<r.samples.size();++k) {
            const auto &sample=r.samples[k];
            const double expected=steady+(.4-steady)*std::exp(-decay*sample.time);
            near(sample.values[omega_channel],expected,
                 method==Method::trapezoidal?2e-7:2e-3,"DC motor analytical speed");
            near(sample.values[current_channel],(12-m.back_emf_constant*sample.values[omega_channel])/motor.value,
                 1e-12,"DC motor armature equation");
            if(k) {
                const auto &prior_sample=r.samples[k-1];
                const double dt=sample.time-prior_sample.time;
                auto integrate=[&](double a,double b){return .5*(a+b)*dt;};
                const double i0=prior_sample.values[current_channel],i1=sample.values[current_channel];
                const double w0=prior_sample.values[omega_channel],w1=sample.values[omega_channel];
                electrical_energy+=integrate(12*i0,12*i1);
                copper_energy+=integrate(motor.value*i0*i0,motor.value*i1*i1);
                damping_energy+=integrate(m.damping*w0*w0,m.damping*w1*w1);
                load_energy+=integrate(m.load_torque*w0,m.load_torque*w1);
            }
        }
        const double w0=r.samples.front().values[omega_channel],w1=r.samples.back().values[omega_channel];
        const double stored=.5*m.inertia*(w1*w1-w0*w0);
        near(electrical_energy-copper_energy-damping_energy-load_energy-stored,0,
             method==Method::trapezoidal?2e-7:2e-4,"DC motor energy balance");
    }

    // The DC operating point includes external shaft load rather than silently
    // dropping it: electromagnetic torque equals damping plus load torque.
    p=motor_project(Method::backward_euler);
    p.profile.initial_state=InitialState::dc_operating_point;
    r=execute(compile(p));
    {
        const auto omega=named_channel(r,11,"omega:");
        const auto current=named_channel(r,11,"i:");
        const double i=r.samples.front().values[current],w=r.samples.front().values[omega];
        near(12,2*i+.2*w,1e-12,"DC motor operating-point voltage");
        near(.2*i,.01*w+.05,1e-12,"DC motor operating-point torque");
    }
    p.components[1].motor.load_torque=1.2;
    r=execute(compile(p));
    near(r.samples.front().values[named_channel(r,11,"omega:")],0,1e-12,
         "DC motor stall speed");
    near(r.samples.front().values[named_channel(r,11,"i:")],6,1e-12,
         "DC motor stall current");

    // Speed and its previous mechanical-force history must survive a checkpoint
    // exactly, including the trapezoidal companion term.
    p=motor_project(Method::trapezoidal);
    const auto motor_ir=compile(p);
    const auto uninterrupted=execute(motor_ir);
    ExecutionOptions stop_after;
    stop_after.capture_snapshot=true;
    stop_after.max_steps=517;
    const auto prefix=execute(motor_ir,nullptr,nullptr,nullptr,nullptr,{},&stop_after);
    require(prefix.snapshot.has_value(),"DC motor snapshot produced");
    ExecutionOptions continue_from;
    continue_from.resume=&*prefix.snapshot;
    const auto resumed=execute(motor_ir,nullptr,nullptr,nullptr,nullptr,{},&continue_from);
    require(resumed.samples.back().values.size()==uninterrupted.samples.back().values.size(),
            "DC motor resumed channel count");
    for(size_t k=0;k<resumed.samples.back().values.size();++k)
        near(resumed.samples.back().values[k],uninterrupted.samples.back().values[k],0,
             "DC motor bit-exact snapshot continuation");

    p=motor_project(Method::backward_euler);
    p.components[1].motor.back_emf_constant=.21;
    error("invalid_parameter",[&]{(void)compile(p);});
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

static Project freewheel(Method method) {
    auto p=base(); p.nodes.push_back({id(5),"switch_output",false});
    p.profile={.015,.00001,method};
    p.components={part(10,Kind::voltage,3,2,10),part(11,Kind::ideal_switch,3,5,0),
        part(12,Kind::resistor,5,4,10),part(13,Kind::inductor,4,2,.1),
        part(14,Kind::diode,2,5,0)};
    p.components[1].closed=true; p.events={{.0053,id(11),false}};
    return p;
}
static void diode_tests() {
    for(auto method:{Method::backward_euler,Method::trapezoidal}) {
        auto p=base(); p.profile.method=method;
        p.components={part(10,Kind::voltage,3,2,5),part(11,Kind::diode,3,4,0),
            part(12,Kind::resistor,4,2,10)};
        auto r=execute(compile(p));
        near(value(r,r.samples.front(),4),5,1e-12,"Forward diode voltage");
        near(value(r,r.samples.front(),11),.5,1e-12,"Forward diode current");
        require(r.max_step_iterations>=2,"Active set finds forward diode state");
        p.components[0].value=-5; r=execute(compile(p));
        near(value(r,r.samples.back(),4),0,1e-12,"Reverse diode voltage");
        near(value(r,r.samples.back(),11),0,1e-12,"Reverse diode current");
        // A current-driven diode has a singular off trial, but a valid on state.
        p=base(); p.nodes.pop_back(); p.profile.method=method;
        p.components={part(10,Kind::current,2,3,2),part(11,Kind::diode,3,2,0)};
        r=execute(compile(p));
        near(value(r,r.samples.front(),3),0,1e-12,"Current driven diode voltage");
        near(value(r,r.samples.front(),11),2,1e-12,"Current driven diode current");
        // RC charging through an ideal diode: same independent analytic curve.
        p=rc(); p.profile.method=method; p.nodes.push_back({id(5),"after_diode",false});
        p.components[1].positive=id(5); p.components.push_back(part(15,Kind::diode,3,5,0));
        r=execute(compile(p));
        for(const auto& sample:r.samples)
            near(value(r,sample,4),1-std::exp(-sample.time/.001),method==Method::trapezoidal?1e-7:.0002,"Diode RC charging");
        p.components[0].value=0; p.components[2].initial=2;
        r=execute(compile(p));
        for(const auto& sample:r.samples) near(value(r,sample,4),2,1e-12,"Reverse diode holds capacitor charge");
        // Bridge is an ordinary atom graph, including floating trial states.
        for(double supply:{5.0,-5.0}) {
            p=base(); p.profile.method=method;
            p.nodes.push_back({id(5),"bridge_minus",false});
            p.components={part(10,Kind::voltage,3,2,supply),part(11,Kind::resistor,4,5,10),
                part(12,Kind::diode,3,4,0),part(13,Kind::diode,2,4,0),
                part(14,Kind::diode,5,3,0),part(15,Kind::diode,5,2,0)};
            r=execute(compile(p));
            const auto& sample=r.samples.back();
            near(value(r,sample,4)-value(r,sample,5),5,1e-12,"Atomic bridge rectifies both polarities");
            for(unsigned n:{12,13,14,15}) require(value(r,sample,n)>=-1e-12,"Diode current is nonnegative");
            const double source_power=supply*value(r,sample,10);
            near(source_power+2.5,0,1e-12,"Bridge energy / source power sign");
            std::reverse(p.components.begin(),p.components.end());
            auto permuted=execute(compile(p));
            require(permuted.samples.back().values==sample.values,"Diode ordering deterministic");
        }
        p=freewheel(method); r=execute(compile(p));
        const double iedge=1-std::exp(-.0053/.01);
        bool saw_edge=false;
        for(const auto& sample:r.samples) {
            const double expected=sample.time<=.0053?1-std::exp(-sample.time/.01):iedge*std::exp(-(sample.time-.0053)/.01);
            near(value(r,sample,13),expected,method==Method::trapezoidal?1e-7:.0002,"Freewheel inductor current");
            if(sample.time==.0053) saw_edge=true;
            if(sample.time>=.0053) {
                near(value(r,sample,5),0,1e-12,"Freewheel diode clamps voltage");
                near(value(r,sample,14),value(r,sample,13),1e-12,"Freewheel diode carries inductor current");
            }
        }
        require(saw_edge,"Freewheel gate edge exact");
        require(r.max_scaled_residual<1e-12,"Diode KCL/KVL residual");
    }
    auto p=base();
    p.components={part(10,Kind::voltage,3,2,5),part(11,Kind::diode,3,4,0),part(12,Kind::resistor,4,2,10)};
    p.profile.max_iterations=1;
    error("nonlinear_convergence",[&]{execute(compile(p));});
    p.profile.max_iterations=0;
    error("invalid_profile",[&]{compile(p);});
    p.profile.max_iterations=64; p.profile.voltage_tolerance=-1;
    error("invalid_profile",[&]{compile(p);});
    p.profile.voltage_tolerance=1e-9; p.components[1].value=1;
    error("invalid_parameter",[&]{compile(p);});
    p=freewheel(Method::trapezoidal);
    p.profile.max_iterations=48; p.profile.current_tolerance=1e-11;
    std::istringstream encoded(saved(p)); const auto q=read_project(encoded);
    require(saved(q)==saved(p),"Diode profile and model round trip");
    auto a=execute(compile(p)),b=execute(compile(q));
    require(a.samples.back().values==b.samples.back().values,"Diode saved simulation repeatability");
}

static void serialization() {
    auto p=switching(); p.name="Unicode схема / quoted \"name\""; p.components[0].x=123.5;
    p.extensions={"x-test future_extension {keep this exactly}"};
    auto text=saved(p); std::istringstream in(text); auto q=read_project(in);
    require(saved(q)==text,"Lossless save/load including UUID, geometry, events, unknown extension");
    std::string windows="\xef\xbb\xbf";
    for(char c:text) { if(c=='\n') windows+='\r'; windows+=c; }
    std::istringstream windows_in(windows);
    require(saved(read_project(windows_in))==text,"UTF-8 BOM and CRLF round trip");
    auto a=execute(compile(p)),b=execute(compile(q));
    require(a.samples.back().values==b.samples.back().values,"Semantics round trip");
    auto code_project=p;
    GatePattern layout_gate{id(29),"Layout Gate",40,20,false};
    layout_gate.script=true;layout_gate.code="for (int ind=0; ind<6; ++ind) IN[ind]=ind%2;";layout_gate.outputs=6;
    layout_gate.pin_positions={{"out",45,-50},{"out5",0,45}};
    code_project.patterns.push_back(layout_gate);
    CodeBlock block;
    block.id=id(30);block.name="PI code";block.x=120;block.y=-40;block.period=1e-4;block.phase=2e-5;
    block.code="static double integral = 0;\nintegral += error * dt;\ncommand = 2 * error + integral;";
    block.inputs={{id(31),"error","V",SignalScalarType::real,0}};
    block.outputs={{id(32),"command","V",SignalScalarType::real,0},
                   {id(33),"gate","",SignalScalarType::boolean,0}};
    block.pin_positions={{id(31),-60,0},{id(32),60,-10},{id(33),60,10}};
    block.icon={{IconPrimitiveKind::polyline,IconColor::signal,false,{},{{2,26},{9,18},{16,8},{23,18},{30,26}}},
                {IconPrimitiveKind::text,IconColor::foreground,false,"PI",{{16,17},{8,0}}}};
    code_project.code_blocks.push_back(block);
    code_project.object_icons.push_back({code_project.components.front().id,
        {{IconPrimitiveKind::rectangle,IconColor::accent,false,{},{{4,4},{28,28}}}}});
    std::istringstream code_input(saved(code_project));
    const auto code_loaded=read_project(code_input);
    require(code_loaded.patterns==code_project.patterns,
            "Multi-output Gate pin layout round trip");
    require(code_loaded.code_blocks==code_project.code_blocks,
            "Typed code-block ports, schedule, layout and multiline C source round trip");
    require(code_loaded.object_icons==code_project.object_icons,"Generic object icon round trip");
    auto incomplete_code_icon=code_project;
    incomplete_code_icon.code_blocks.front().icon.front().points.resize(1);
    error("invalid_code_icon",[&]{(void)saved(incomplete_code_icon);});
    auto incomplete_object_icon=code_project;
    incomplete_object_icon.object_icons.front().primitives.front().points.resize(1);
    error("invalid_object_icon",[&]{(void)saved(incomplete_object_icon);});
    auto malformed_icon=saved(code_project);
    const auto icon_record=malformed_icon.find("x-code-icon ");
    const auto point_count=malformed_icon.find("\"\" 5 ",icon_record);
    require(icon_record!=std::string::npos&&point_count!=std::string::npos,
            "Serialized code icon record is available for parser validation");
    malformed_icon.replace(point_count+3,1,"1");
    error("parse_error",[&]{std::istringstream input(malformed_icon);(void)read_project(input);});
    auto invalid_code=code_project;invalid_code.code_blocks.front().outputs.back().initial=2;
    error("invalid_signal_port",[&]{(void)saved(invalid_code);});
    auto expression_project=p;
    expression_project.initialization_code=
        "double accumulate(int count) { double value = 0; for (int i = 0; i < count; ++i) value += 1; return value; }\n"
        "double base = accumulate(4);\n// UTF-8: параметры\ndouble multiplier = 0;\n"
        "if (base == 4) { multiplier = 3; } else { multiplier = 1; }";
    expression_project.parameter_expressions={{id(14),"value","base * multiplier"}};
    std::istringstream expression_input(saved(expression_project));const auto expression_loaded=read_project(expression_input);
    require(expression_loaded.initialization_code==expression_project.initialization_code&&
            expression_loaded.parameter_expressions==expression_project.parameter_expressions,
            "Initialization source and parameter expressions round trip");
    const auto resolved=resolve_parameter_expressions(expression_loaded);
    require(resolved.components.back().value==12,"Parameter expression changes the compiled numeric property");
    CProgramOptions bounded_options;bounded_options.instruction_budget=100;bounded_options.diagnostic_code="bounded_c";
    error("bounded_c",[&]{const auto endless=compile_c_program("while (true) {}",bounded_options);(void)execute_c_program(endless);});
    error("bounded_c",[&]{const auto invalid_shift=compile_c_program("return 1 << 64;",bounded_options);(void)execute_c_program(invalid_shift);});
    CProgramOptions returning_options;returning_options.diagnostic_code="bounded_c";returning_options.require_return=true;
    error("bounded_c",[&]{const auto nonfinite=compile_c_program("return log(-1);",returning_options);(void)execute_c_program(nonfinite);});
    error("bounded_c",[&]{(void)compile_c_program("if (false) return missing_name; return 0;",returning_options);});
    const auto cast_program=compile_c_program("double angle = M_PI; return (int)(angle > 3.0) + 1u;",returning_options);
    require(execute_c_program(cast_program).return_value==2,"C scalar casts, suffixes and math constants");
    returning_options.allow_time=true;returning_options.allow_gate_functions=true;
    const auto ramp_program=compile_c_program(
        "double curr_ramp = ramp(0, 10, 0.008333333, 0.001111111); "
        "return phasepwm(50, 0.02, curr_ramp) + (stime == t ? 0 : 10);",returning_options);
    require(execute_c_program(ramp_program,5).return_value.has_value(),
            "Gate C accepts local ramp variables and stime aliases the current invocation time");
    CProgramOptions common_options;common_options.diagnostic_code="common_c";
    common_options.require_return=true;common_options.allow_time=true;
    const auto common_program=compile_c_program(
        "return ramp(0, 2, 0, 10) + pulse(1, 1) + saw(1, 0) + triangle(1, 0) + "
        "lerp(2, 4, 0.5) + saturate(2) + sign(-3) + step(2, 2) + "
        "smoothstep(0, 1, 0.5) + deadband(3, 1) + wrap(-1, 4);",common_options);
    const auto common_result=execute_c_program(common_program,1.5).return_value;
    require(common_result&&std::abs(*common_result-19.5)<1e-12,
            "Common signal functions are available without Gate-only options");
    CProgramOptions ports;ports.diagnostic_code="code_ports";
    ports.external_variables={"error","dt","command"};ports.writable_variables={"command"};
    const auto pi=compile_c_program("static double integral = 0; integral += error * dt; command = 2 * error + integral;",ports);
    CProgramState pi_state;
    auto first=execute_c_program(pi,0,&pi_state,{{"error",3},{"dt",.1},{"command",0}});
    auto second=execute_c_program(pi,.1,&pi_state,{{"error",2},{"dt",.1},{"command",first.variables.at("command")}});
    near(first.variables.at("command"),6.3,1e-15,"C external output and static state");
    near(second.variables.at("command"),4.5,1e-15,"C external inputs and persistent state");
    error("code_ports",[&]{(void)compile_c_program("error = 1;",ports);});
    error("code_ports",[&]{(void)execute_c_program(pi,0,&pi_state,{{"unknown",1}});});
    CProgramOptions arrays;arrays.diagnostic_code="code_arrays";arrays.external_arrays["IN"]=6;arrays.writable_arrays.insert("IN");
    const auto six_gates=compile_c_program("for (int ind = 0; ind < 6; ++ind) IN[ind] = ind % 2;",arrays);
    const auto outputs=execute_c_program(six_gates);
    for(int ind=0;ind<6;++ind)require(outputs.variables.at("IN["+std::to_string(ind)+"]")==ind%2,"Checked writable C array");
    error("code_arrays",[&]{const auto out_of_range=compile_c_program("IN[6] = 1;",arrays);(void)execute_c_program(out_of_range);});
    arrays.writable_arrays.clear();
    error("code_arrays",[&]{(void)compile_c_program("IN[0] = 1;",arrays);});
    error("bounded_c",[&]{(void)compile_c_program("return "+std::string(300,'!')+"true;",returning_options);});
    auto invalid=expression_loaded;invalid.parameter_expressions={{id(999),"value","base"}};
    error("invalid_parameter_expression",[&]{compile(invalid);});
    error("schema_version",[]{std::istringstream s("PowerDriveSim 99\n"); read_project(s);});
    error("parse_error",[]{std::istringstream s(""); read_project(s);});
    error("parse_error",[]{std::istringstream s("PowerDriveSim\n"); read_project(s);});
    error("parse_error",[]{std::istringstream s("PowerDriveSim 1\nunknown 5\n"); read_project(s);});
    error("parse_error",[&]{std::istringstream s(text+"profile 1 0.1\n"); read_project(s);});
    error("parse_error",[&]{std::istringstream s(text+"event 0.01 \"id\" 2\n"); read_project(s);});
    p.profile.method=Method::trapezoidal;
    auto method_text=saved(p); std::istringstream method_in(method_text);
    require(read_project(method_in).profile.method==Method::trapezoidal,"Method round trip");
    {
        auto transformer=base();
        transformer.components={part(10,Kind::voltage,3,2,10),part(11,Kind::ideal_transformer,3,2,2),
                                part(12,Kind::resistor,4,2,8)};
        transformer.components[1].secondary_positive=id(4);
        transformer.components[1].secondary_negative=id(2);
        const auto encoded=saved(transformer);
        std::istringstream input(encoded);
        const auto restored=read_project(input);
        require(restored.components[1].secondary_positive==id(4) &&
                restored.components[1].secondary_negative==id(2),"Transformer four terminals round trip");
        require(saved(restored)==encoded,"Transformer serialization is stable");
        const auto wired=make_wired(transformer);
        require(wired.wires.size()==8 &&
                wired.components[1].secondary_positive.empty() &&
                wired.components[1].secondary_negative.empty(),
                "Legacy transformer nets convert to four explicit pins");
        const auto wired_result=execute(compile(wired));
        near(value(wired_result,wired_result.samples.back(),4),5,1e-12,
             "Converted transformer retains its secondary voltage");
    }
    {
        auto motor=base();
        motor.components={part(10,Kind::voltage,3,2,10),part(11,Kind::dc_motor,3,2,2)};
        motor.components[1].motor={.2,.2,.02,.01,.05};
        const auto encoded=saved(motor);
        std::istringstream input(encoded);
        const auto restored=read_project(input);
        require(restored.components[1].motor==motor.components[1].motor,
                "DC motor mechanical parameters round trip");
        require(saved(restored)==encoded,"DC motor serialization is stable");
        auto incomplete=encoded;
        const auto start=incomplete.find("dc_motor ");
        require(start!=std::string::npos,"DC motor record is serialized");
        incomplete.erase(start,incomplete.find('\n',start)-start+1);
        error("invalid_parameter",[&]{std::istringstream missing(incomplete);read_project(missing);});
    }

    // CTest uses build as cwd, so migration also has a self-contained fixture.
    auto downgrade_nodes=[](std::string original) {
        std::istringstream input(original); std::string line,output;
        while(std::getline(input,line)) {
            if(line.rfind("scope_enabled ",0)==0 || line.rfind("wiring ",0)==0 || line.rfind("scopeview ",0)==0 || line.rfind("initialization ",0)==0 || line.rfind("stepping ",0)==0 || line.rfind("expression_init ",0)==0 || line.rfind("parameter_expression ",0)==0) continue;
            if(line.rfind("node ",0)==0) {
                line.erase(line.find_last_of(' ')); line.erase(line.find_last_of(' '));
            }
            output+=line+'\n';
        }
        return output;
    };
    std::string old=downgrade_nodes(text); old.replace(0,old.find('\n'),"PowerDriveSim 1");
    const auto nonlinear_at=old.find("nonlinear ");
    old.erase(nonlinear_at,old.find('\n',nonlinear_at)-nonlinear_at+1);
    const auto method_at=old.find(" BackwardEuler");
    require(method_at!=std::string::npos,"Fixture contains v2 method");
    old.erase(method_at,14);
    std::istringstream old_in(old); auto migrated=read_project(old_in);
    require(migrated.schema==project_schema && migrated.profile.method==Method::backward_euler,"Explicit v1 migration");
    require(migrated.extensions==p.extensions,"Migration preserves unknown extensions");
    std::string v2=downgrade_nodes(text); v2.replace(0,v2.find('\n'),"PowerDriveSim 2");
    const auto v2_nonlinear=v2.find("nonlinear ");
    v2.erase(v2_nonlinear,v2.find('\n',v2_nonlinear)-v2_nonlinear+1);
    std::istringstream v2_in(v2); const auto migrated_v2=read_project(v2_in);
    require(migrated_v2.schema==project_schema && migrated_v2.profile.max_iterations==64,"Explicit v2 migration");
    require(saved(migrated_v2)==text,"Migration v2 preserves entire semantics");
    error("parse_error",[]{std::istringstream s("PowerDriveSim 3\nproject id name\nprofile 1 .1 BackwardEuler\n"); read_project(s);});
    error("invalid_method",[]{parse_method("magic");});
    p.extensions={"x-invalid\nnode"};
    error("extension_error",[&]{saved(p);});
    p=switching(); p.name="invalid\nname";
    error("invalid_text",[&]{saved(p);});
    p=switching();
    std::ostringstream flags; flags << std::boolalpha << std::fixed;
    write_project(p,flags); require(flags.str()==saved(p),"Format independent of numeric stream flags");
}
static void topology() {
    auto p=rc(); p.nodes[0].ground=false;
    error("missing_ground",[&]{compile(p);});
    p=rc(); p.nodes.push_back({id(20),"floating",false});
    error("floating_node",[&]{compile(p);});
    p=rc(); p.components[1].name="IP1"; p.components[1].positive=id(999);
    try {
        compile(p);
        require(false,"Disconnected component must be diagnosed");
    } catch(const Diagnostic& diagnostic) {
        require(diagnostic.code=="missing_terminal"&&diagnostic.object==p.components[1].id&&
                std::string(diagnostic.what()).find("IP1")!=std::string::npos,
                "Missing terminal keeps its UUID and includes its visible name");
    }
    p=rc(); p.components[1].name="IP2"; p.components[1].negative=p.components[1].positive;
    try {
        compile(p);
        require(false,"Shorted component must be diagnosed");
    } catch(const Diagnostic& diagnostic) {
        require(diagnostic.code=="shorted_component"&&diagnostic.object==p.components[1].id&&
                std::string(diagnostic.what()).find("IP2")!=std::string::npos,
                "Shorted component keeps its UUID and includes its visible name");
    }
    p=rc(); p.components[1].value=0;
    error("invalid_parameter",[&]{compile(p);});
    p=rc(); p.profile.step=std::numeric_limits<double>::infinity();
    error("invalid_profile",[&]{compile(p);});
    p=rc(); p.components[1].id=p.components[0].id;
    error("invalid_uuid",[&]{compile(p);});
    p=rc(); p.components.push_back(part(20,Kind::voltage,3,2,2));
    error("conflicting_voltage_constraints",[&]{execute(compile(p));});
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
    error("floating_island",[&]{execute(compile(p));});

    // Equal ideal sources still have undetermined individual branch currents.
    p=rc(); p.components.push_back(part(20,Kind::voltage,3,2,1));
    error("ideal_voltage_loop",[&]{compile(p);});
    // Three-edge loops exercise signed potentials, not just parallel branches.
    p=base(); p.components={part(10,Kind::voltage,3,2,5),part(11,Kind::voltage,4,3,2),
                           part(12,Kind::voltage,4,2,7)};
    error("ideal_voltage_loop",[&]{compile(p);});
    p.components.back().value=-7;
    error("conflicting_voltage_constraints",[&]{compile(p);});
    std::reverse(p.components.begin(),p.components.end());
    error("conflicting_voltage_constraints",[&]{compile(p);});
    p=base(); p.nodes.pop_back();
    p.components={part(10,Kind::current_probe,3,2,0),part(11,Kind::current_probe,2,3,0)};
    error("ideal_voltage_loop",[&]{compile(p);});

    // Capacitor constraints exist at initialization, not on ordinary steps.
    p=base(); p.nodes.pop_back();
    p.components={part(10,Kind::voltage,3,2,5),part(20,Kind::capacitor,3,2,1e-6,0)};
    error("conflicting_voltage_constraints",[&]{execute(compile(p));});
    p.components.back().initial=5;
    error("ideal_voltage_loop",[&]{execute(compile(p));});

    // A disconnected inductor initial current cannot disappear through an open switch.
    p=base(); p.nodes.pop_back();
    p.components={part(10,Kind::inductor,3,2,.01,1),part(11,Kind::ideal_switch,3,2,0)};
    try { execute(compile(p)); require(false,"Expected current cutset"); }
    catch(const Diagnostic& d) {
        require(d.code=="current_cutset" && d.object==id(10) && d.time==0,
                "Current cutset identifies the inductor and time");
    }
    p.components.front().initial=0;
    error("floating_island",[&]{execute(compile(p));});
    p.components.back().closed=true;
    require(!execute(compile(p)).samples.empty(),"Closed switch provides inductor return path");

    // An event can make a previously valid topology impossible.
    p=base(); p.nodes.pop_back(); p.profile={.002,.0001};
    p.components={part(10,Kind::voltage,3,2,5),part(20,Kind::ideal_switch,3,2,0)};
    p.events={{.001,id(20),true}};
    try { execute(compile(p)); require(false,"Expected shorted voltage source"); }
    catch(const Diagnostic& d) {
        require(d.code=="conflicting_voltage_constraints" && d.object==id(20) && d.time==.001,
                "Gate-induced conflict identifies switch and event time");
    }
    p.components={part(10,Kind::current,2,3,1),part(20,Kind::ideal_switch,3,2,0)};
    p.components.back().closed=true; p.events={{.001,id(20),false}};
    try { execute(compile(p)); require(false,"Expected interrupted current source"); }
    catch(const Diagnostic& d) {
        require(d.code=="current_cutset" && d.object==id(10) && d.time==.001,
                "Gate-induced current cutset retains the event time");
    }
}
static void regression() {
    // Compare cache hits, eviction, pivoting and sparse fallback to the original solver.
    for (size_t n : {size_t(3), size_t(8), size_t(65)}) {
        FactorizationCache cache;
        SimulationIR matrix_ir;
        matrix_ir.unknowns.resize(n);
        for (size_t variant=0; variant<22; ++variant) {
            StampSystem system(n);
            for (size_t r=0; r<n; ++r) {
                system.add(static_cast<int>(r),static_cast<int>((r+1)%n),10+static_cast<double>(variant));
                system.add(static_cast<int>(r),static_cast<int>(r),.5);
                if(n>3)system.add(static_cast<int>(r),static_cast<int>((r+3)%n),-.25);
            }
            for(int repeat=0;repeat<3;++repeat) {
                for(size_t r=0;r<n;++r)system.rhs[r]=std::sin(double(r+repeat));
                auto expected=system.solve(matrix_ir,0), actual=cache.solve(system,matrix_ir,0);
                for(size_t r=0;r<n;++r)near(actual[r],expected[r],1e-14,"Cached factorization matches sparse solve");
            }
        }
        StampSystem singular(n);
        error("singular_matrix",[&]{cache.solve(singular,matrix_ir,0);});
        singular.add(0,0,std::numeric_limits<double>::infinity());
        error("nonfinite_stamp",[&]{cache.solve(singular,matrix_ir,0);});
    }
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
    for(const auto& name:{"rc","rlc","switch","rc-trapezoidal","diode-freewheel","transformer-yy"}) {
        std::ifstream in(root+"/examples/"+name+".pds"); auto p=read_project(in); auto r=execute(compile(p));
        require(!r.samples.empty() && r.samples.back().time==p.profile.stop,"Example completes");
        if(std::string(name)=="rc" || std::string(name)=="rc-trapezoidal") near(value(r,r.samples.back(),4),1-std::exp(-5),0.0002,"RC example");
        if(std::string(name)=="rlc") {
            double t=p.profile.stop;
            near(value(r,r.samples.back(),4),1-std::exp(-100*t)*(std::cos(300*t)+std::sin(300*t)/3),0.001,"RLC example");
        }
        if(std::string(name)=="diode-freewheel") near(value(r,r.samples.back(),13),(1-std::exp(-.53))*std::exp(-.97),1e-7,"Freewheel example");
        if(std::string(name)=="switch") near(value(r,r.samples.back(),4),0,1e-12,"Switch example");
        if(std::string(name)=="transformer-yy") {
            const auto sample=std::find_if(r.samples.begin(),r.samples.end(),[](const Sample& s) {
                return std::abs(s.time-.005)<1e-12;
            });
            require(sample!=r.samples.end(),"Three-phase transformer sample at voltage peak");
            auto channel_value=[&](const Result& result,const Sample& point,const std::string& label) {
                const auto found=std::find_if(result.channels.begin(),result.channels.end(),[&](const Channel& c) {
                    return c.name==label;
                });
                require(found!=result.channels.end(),"Three-phase transformer channel "+label);
                return point.values[size_t(found-result.channels.begin())];
            };
            double source_power=0,load_power=0;
            int phase_index=0;
            for(const auto& phase:{"A","B","C"}) {
                const double offset=phase_index==0?0:phase_index==1?-2*std::numbers::pi/3:
                                                                             2*std::numbers::pi/3;
                const double primary=10*std::sin(2*std::numbers::pi*50*sample->time+offset);
                const double secondary=channel_value(r,*sample,std::string("u:Voltage ")+phase);
                const double current=channel_value(r,*sample,std::string("i:Source ")+phase);
                near(secondary,primary/2,1e-12,"Three-phase transformer phase ratio");
                source_power-=primary*current;
                load_power+=secondary*secondary/8;
                ++phase_index;
            }
            near(source_power,load_power,1e-12,"Three-phase transformer instant power balance");
            p.instances[0].parameters={{p.definitions[0].parameters[0].id,4}};
            const auto changed=execute(compile(p));
            const auto peak=std::find_if(changed.samples.begin(),changed.samples.end(),[](const Sample& s) {
                return std::abs(s.time-.005)<1e-12;
            });
            require(peak!=changed.samples.end(),"Overridden transformer sample");
            near(channel_value(changed,*peak,"u:Voltage A"),2.5,1e-12,
                 "Public Np/Ns parameter changes all three phases");
        }
    }
}
int main(int argc,char** argv) {
    try {
        std::string group=argc>1?argv[1]:"all",root=argc>2?argv[2]:".";
        if(group=="unit" || group=="all") unit();
        if(group=="numerical" || group=="all") { numerical(); trapezoidal_tests(); }
        if(group=="diode" || group=="all") diode_tests();
        if(group=="serialization" || group=="all") serialization();
        if(group=="topology" || group=="all") topology();
        if(group=="regression" || group=="all") regression();
        if(group=="examples" || group=="all") examples(root);
        require(checks>0,"Unknown test group");
        std::cout<<"PASS "<<group<<" checks="<<checks<<'\n'; return 0;
    } catch(const Diagnostic& d) { std::cerr<<d.code<<" object="<<d.object<<" time="<<d.time<<" "<<d.what()<<'\n'; return 1;
    } catch(const std::exception& e) { std::cerr<<"FAIL "<<e.what()<<'\n'; return 1; }
}
