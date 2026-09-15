#include "core/solver/reference/reference.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace pds {
void StampSystem::add(int r,int c,double v) { if(r>=0 && c>=0 && v!=0.0) rows.at(r)[c]+=v; }
void StampSystem::inject(int r,double v) { if(r>=0) rhs.at(r)+=v; }
void StampSystem::incidence(int p,int n,int b) { add(p,b,1); add(n,b,-1); }
void StampSystem::conductance(int p,int n,double g) { add(p,p,g); add(n,n,g); add(p,n,-g); add(n,p,-g); }
std::vector<double> StampSystem::solve(const SimulationIR& ir,double time) const {
    auto a=rows; auto b=rhs; const int n=static_cast<int>(b.size());
    auto get=[&](int r,int c) { auto it=a[r].find(c); return it==a[r].end()?0.0:it->second; };
    for(int r=0;r<n;++r) {
        double scale=0;
        for(const auto& entry:a[r]) {
            if(!std::isfinite(entry.second)) throw Diagnostic("nonfinite_stamp",ir.unknowns[r].object,"Nonfinite equation coefficient",time);
            scale=std::max(scale,std::abs(entry.second));
        }
        if(!std::isfinite(b[r])) throw Diagnostic("nonfinite_stamp",ir.unknowns[r].object,"Nonfinite equation right hand side",time);
        if(scale>0) { for(auto& entry:a[r]) entry.second/=scale; b[r]/=scale; }
    }
    for(int k=0;k<n;++k) {
        int pivot=k;
        for(int r=k+1;r<n;++r) if(std::abs(get(r,k))>std::abs(get(pivot,k))) pivot=r;
        if(std::abs(get(pivot,k))<1e-14)
            throw Diagnostic("singular_matrix",ir.unknowns[k].object,"Singular or ill-conditioned equations: check floating islands, ideal loops and initial conditions; no stabilizer was added",time);
        std::swap(a[k],a[pivot]); std::swap(b[k],b[pivot]);
        const double diagonal=get(k,k);
        for(int r=k+1;r<n;++r) {
            double f=get(r,k)/diagonal;
            if(f==0) continue;
            a[r].erase(k);
            for(const auto& [column,value]:a[k]) if(column>k) a[r][column]-=f*value;
            b[r]-=f*b[k];
        }
    }
    std::vector<double> x(n,0);
    for(int r=n-1;r>=0;--r) {
        double v=b[r];
        for(const auto& [column,value]:a[r]) if(column>r) v-=value*x[column];
        x[r]=v/get(r,r);
        if(!std::isfinite(x[r])) throw Diagnostic("nonfinite_solution",ir.unknowns[r].object,"Solution is nonfinite",time);
    }
    return x;
}
Result execute(const SimulationIR& ir,const std::atomic_bool* cancel) {
    if(ir.unknowns.empty() || !std::isfinite(ir.profile.step) || ir.profile.step<=0 || !std::isfinite(ir.profile.stop) || ir.profile.stop<=0)
        throw Diagnostic("invalid_ir",ir.project_id,"IR must have unknowns and a positive finite time profile");
    (void)method_name(ir.profile.method);
    Result result; result.project_id=ir.project_id; result.profile=ir.profile; result.channels=ir.unknowns;
    std::vector<double> states(ir.stamps.size()), history(ir.stamps.size());
    const bool trapezoidal=ir.profile.method==Method::trapezoidal;
    std::vector<bool> gates(ir.stamps.size());
    for(size_t i=0;i<ir.stamps.size();++i) {
        states[i]=ir.stamps[i].component.initial;
        gates[i]=ir.stamps[i].component.closed;
        if(ir.stamps[i].component.kind==Kind::ideal_switch) result.gate_objects.push_back(ir.stamps[i].component.id);
    }
    size_t next_event=0;
    auto apply_events=[&](double t) {
        bool changed=false;
        while(next_event<ir.events.size() && ir.events[next_event].time==t) {
            const auto& e=ir.events[next_event++];
            for(size_t i=0;i<ir.stamps.size();++i) if(ir.stamps[i].component.id==e.target) gates[i]=e.closed;
            changed=true;
        }
        return changed;
    };
    auto solve=[&](double t,double h,bool initialize) {
        StampSystem system(ir.unknowns.size());
        for(size_t i=0;i<ir.stamps.size();++i) {
            const auto& s=ir.stamps[i]; const auto& c=s.component;
            int p=s.positive,n=s.negative,b=s.branch;
            if(b>=0) system.incidence(p,n,b);
            switch(c.kind) {
            case Kind::resistor: system.conductance(p,n,1/c.value); break;
            case Kind::current: system.inject(p,-c.value); system.inject(n,c.value); break;
            case Kind::voltage: system.add(b,p,1); system.add(b,n,-1); system.inject(b,c.value); break;
            case Kind::capacitor:
                system.add(b,p,1); system.add(b,n,-1);
                if(!initialize) system.add(b,b,-h/(c.value*(trapezoidal?2:1)));
                system.inject(b,states[i]+(!initialize && trapezoidal?h/(2*c.value)*history[i]:0)); break;
            case Kind::inductor:
                if(initialize) { system.add(b,b,1); system.inject(b,states[i]); }
                else {
                    const double factor=c.value/h*(trapezoidal?2:1);
                    system.add(b,p,1); system.add(b,n,-1); system.add(b,b,-factor);
                    system.inject(b,-factor*states[i]-(trapezoidal?history[i]:0));
                }
                break;
            case Kind::ideal_switch:
                if(gates[i]) { system.add(b,p,1); system.add(b,n,-1); }
                else system.add(b,b,1);
                break;
            }
        }
        auto values=system.solve(ir,t);
        for(size_t r=0;r<system.rows.size();++r) {
            double residual=-system.rhs[r],scale=std::abs(system.rhs[r]);
            for(const auto& [column,coefficient]:system.rows[r]) { residual+=coefficient*values[column]; scale+=std::abs(coefficient*values[column]); }
            const double scaled=std::abs(residual)/(1+scale);
            result.max_scaled_residual=std::max(result.max_scaled_residual,scaled);
            if(scaled>1e-9) throw Diagnostic("large_residual",ir.unknowns[r].object,"Linear solve failed residual verification",t);
        }
        for(size_t i=0;i<ir.stamps.size();++i) {
            const auto& s=ir.stamps[i];
            const double voltage=(s.positive<0?0:values[s.positive])-(s.negative<0?0:values[s.negative]);
            if(s.component.kind==Kind::capacitor) { states[i]=voltage; history[i]=values[s.branch]; }
            if(s.component.kind==Kind::inductor) { states[i]=values[s.branch]; history[i]=voltage; }
        }
        return values;
    };
    auto record=[&](double t,std::vector<double> values) {
        Sample sample{t,std::move(values),{}};
        for(size_t i=0;i<ir.stamps.size();++i) if(ir.stamps[i].component.kind==Kind::ideal_switch) sample.gates.push_back(gates[i]);
        result.samples.push_back(std::move(sample));
    };
    apply_events(0);
    record(0,solve(0,0,true));
    double time=0;
    size_t grid=1;
    while(time<ir.profile.stop) {
        if(cancel && cancel->load()) { result.cancelled=true; break; }
        const double grid_time=static_cast<double>(grid)*ir.profile.step;
        double end=std::min(grid_time,ir.profile.stop);
        if(next_event<ir.events.size()) end=std::min(end,ir.events[next_event].time);
        if(end<=time) throw Diagnostic("time_resolution",ir.project_id,"Time step cannot advance floating-point time",time);
        auto values=solve(end,end-time,false);
        ++result.accepted_steps;
        time=end;
        if(time==grid_time) ++grid;
        // Integrate to the edge with the old topology. Apply all simultaneous
        // gates before solving algebraic variables with continuous C/L states.
        if(apply_events(time)) values=solve(time,0,true);
        record(time,std::move(values));
    }
    return result;
}
}