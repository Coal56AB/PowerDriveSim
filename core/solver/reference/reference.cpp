#include "core/solver/reference/reference.hpp"
#include "core/solver/reference/equation_cache.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <deque>
#include <set>
#include <sstream>
#include <thread>
#include <chrono>
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
std::vector<Channel> available_channels(const SimulationIR& ir){
    auto channels=ir.unknowns;for(const auto& o:ir.observations)channels.push_back(o.channel);
    for(const auto& s:ir.stamps)if(s.component.kind==Kind::ideal_switch)channels.push_back({"gate/"+s.component.id,"gate:"+s.component.name,"bool"});
    for(const auto& signal:ir.gate_signals)channels.push_back({"gate/"+signal.id,signal.name,"bool"});
    return channels;
}
Result select_result(const Result& source,const std::vector<std::string>& keys){
    std::set<std::string> selected(keys.begin(),keys.end());Result result;
    result.project_id=source.project_id;result.backend=source.backend;result.precision=source.precision;result.engine=source.engine;result.profile=source.profile;
    result.accepted_steps=source.accepted_steps;result.linear_solves=source.linear_solves;result.max_step_iterations=source.max_step_iterations;result.cancelled=source.cancelled;result.max_scaled_residual=source.max_scaled_residual;result.last_time=source.last_time;
    std::vector<size_t> analog,gates;
    for(size_t i=0;i<source.channels.size();++i)if(selected.count(source.channels[i].object)){analog.push_back(i);result.channels.push_back(source.channels[i]);}
    for(size_t i=0;i<source.gate_objects.size();++i)if(selected.count("gate/"+source.gate_objects[i])){gates.push_back(i);result.gate_objects.push_back(source.gate_objects[i]);}
    if(analog.empty()&&gates.empty())return result;
    result.samples.reserve(source.samples.size());
    for(const auto& old:source.samples){Sample sample;sample.time=old.time;for(auto i:analog)sample.values.push_back(old.values[i]);for(auto i:gates)sample.gates.push_back(old.gates[i]);result.samples.push_back(std::move(sample));}
    return result;
}
static Result execute_impl(const SimulationIR& ir,const std::atomic_bool* cancel,std::atomic<double>* simulated_time,const Recording* recording,const std::atomic_bool* paused,const std::function<void(Result&&)>& stream) {
    if(ir.unknowns.empty() || !std::isfinite(ir.profile.step) || ir.profile.step<=0 || !std::isfinite(ir.profile.stop) || ir.profile.stop<=0)
        throw Diagnostic("invalid_ir",ir.project_id,"IR must have unknowns and a positive finite time profile");
    (void)method_name(ir.profile.method);
    Result result; result.project_id=ir.project_id; result.profile=ir.profile;
    const std::set<std::string> requested=recording?std::set<std::string>(recording->channels.begin(),recording->channels.end()):std::set<std::string>();
    auto selected=[&](const std::string& key){return !recording||recording->all||requested.count(key);};
    std::vector<size_t> analog_indices,gate_indices;
    auto catalog=available_channels(ir);
    if(recording&&!recording->all)for(const auto& key:requested)
        if(std::none_of(catalog.begin(),catalog.end(),[&](const Channel& c){return c.object==key;}))throw Diagnostic("missing_recording_channel",key,"Recording channel does not exist");
    const size_t analog_count=ir.unknowns.size()+ir.observations.size();
    for(size_t i=0;i<analog_count;++i)if(selected(catalog[i].object)){analog_indices.push_back(i);result.channels.push_back(catalog[i]);}
    for(size_t i=analog_count;i<catalog.size();++i)if(selected(catalog[i].object)){gate_indices.push_back(i-analog_count);result.gate_objects.push_back(catalog[i].object.substr(5));}
    std::vector<size_t> switch_indices;
    std::vector<bool> signal_values;for(const auto& signal:ir.gate_signals)signal_values.push_back(signal.initial);
    std::vector<double> states(ir.stamps.size()), history(ir.stamps.size());
    std::vector<bool> gates(ir.stamps.size()), diode_states(ir.stamps.size());
    std::vector<size_t> diode_indices;
    for(size_t i=0;i<ir.stamps.size();++i)
        if(ir.stamps[i].component.kind==Kind::diode) diode_indices.push_back(i);
    for(size_t i=0;i<ir.stamps.size();++i) {
        states[i]=ir.stamps[i].component.initial;
        gates[i]=ir.stamps[i].component.closed;
        if(ir.stamps[i].component.kind==Kind::ideal_switch) switch_indices.push_back(i);
    }
    size_t next_event=0;
    auto apply_events=[&](double t) {
        bool changed=false;
        while(next_event<ir.events.size() && ir.events[next_event].time==t) {
            const auto& e=ir.events[next_event++];
            for(size_t i=0;i<ir.stamps.size();++i) if(ir.stamps[i].component.id==e.target){gates[i]=e.closed;changed=true;}
            for(size_t i=0;i<ir.gate_signals.size();++i)if(ir.gate_signals[i].id==e.target)signal_values[i]=e.closed;
        }
        return changed;
    };
    EquationCache equations(ir);
    std::vector<size_t> violations;
    violations.reserve(diode_indices.size());
    auto solve=[&](double t,double h,bool initialize) -> const std::vector<double>& {
        const std::vector<double>* solution=nullptr;
        bool converged=false, singular=false;
        unsigned iterations=0;
        std::string offending, last_linear;
        double residual_v=0, residual_i=0;
        auto attempt=[&](const std::vector<bool>& active) {
            ++iterations;
            ++result.linear_solves;
            singular=false;
            violations.clear();
            try {
                solution=&equations.solve(t,h,initialize,gates,active,states,history);
            } catch(const Diagnostic& d) {
                if(d.code!="singular_matrix" || diode_indices.empty())throw;
                offending=d.object;last_linear=d.what();singular=true;
                return false;
            }
            const auto &values=*solution;
            residual_v=0;residual_i=0;
            for(size_t index:diode_indices) {
                const auto& stamp=ir.stamps[index];
                const double up=stamp.positive<0?0:values[stamp.positive];
                const double un=stamp.negative<0?0:values[stamp.negative];
                const double v=up-un,current=values[stamp.branch];
                const double vt=ir.profile.voltage_tolerance+ir.profile.relative_tolerance*std::max(std::abs(up),std::abs(un));
                const double it=ir.profile.current_tolerance+ir.profile.relative_tolerance*std::abs(current);
                residual_v=std::max(residual_v,std::max(0.0,v));
                residual_i=std::max(residual_i,std::max(0.0,-current));
                if((active[index]&&current < -it)||(!active[index]&&v>vt)) {
                    violations.push_back(index);offending=stamp.component.id;
                }
            }
            return violations.empty();
        };
        // Most steps retain a valid active set. Allocate search containers only
        // when this trial actually fails; keep the same bounded search order.
        if(diode_indices.empty()||ir.profile.max_iterations>0)converged=attempt(diode_states);
        if(!converged) {
            std::deque<std::vector<bool>> pending;
            std::set<std::vector<bool>> visited;
            visited.insert(diode_states);
            auto enqueue=[&](const std::vector<bool>& active) {
                if(!singular) {
                    auto simultaneous=active;
                    for(size_t index:violations)simultaneous[index]=!simultaneous[index];
                    if(!visited.count(simultaneous))pending.push_front(std::move(simultaneous));
                }
                const auto &indices=singular?diode_indices:violations;
                for(size_t index:indices) {
                    auto neighbor=active;neighbor[index]=!neighbor[index];
                    if(!visited.count(neighbor)&&pending.size()<ir.profile.max_iterations)pending.push_back(std::move(neighbor));
                }
            };
            enqueue(diode_states);
            while(!pending.empty()&&iterations<ir.profile.max_iterations) {
                auto active=std::move(pending.front());pending.pop_front();
                if(!visited.insert(active).second)continue;
                if(attempt(active)){diode_states=std::move(active);converged=true;break;}
                enqueue(active);
            }
        }
        result.max_step_iterations=std::max(result.max_step_iterations,static_cast<size_t>(iterations));
        if(!converged) {
            std::ostringstream message;
            message << "Diode active-set solve failed after " << iterations
                << " iterations; voltage residual=" << residual_v << " V; current residual=" << residual_i
                << " A. Check ideal loops, initial states, tolerances and iteration budget. " << last_linear;
            throw Diagnostic("nonlinear_convergence",offending.empty()?ir.project_id:offending,message.str(),t);
        }
        const auto &system=equations.system();
        const auto &values=*solution;
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
    auto record=[&](double t,const std::vector<double>& values) {
        result.last_time=t;
        if(simulated_time) simulated_time->store(t,std::memory_order_relaxed);
        // No sample or history allocation when recording is disabled.
        if(analog_indices.empty()&&gate_indices.empty())return;
        Sample sample;sample.time=t;sample.values.reserve(analog_indices.size());sample.gates.reserve(gate_indices.size());
        for(size_t index:analog_indices){
            if(index<ir.unknowns.size())sample.values.push_back(values[index]);
            else {const auto& o=ir.observations[index-ir.unknowns.size()];sample.values.push_back(((o.positive<0?0:values[o.positive])-(o.negative<0?0:values[o.negative]))*o.gain+o.offset);}
        }
        for(size_t index:gate_indices)sample.gates.push_back(index<switch_indices.size()?gates[switch_indices[index]]:signal_values[index-switch_indices.size()]);
        result.samples.push_back(std::move(sample));
    };
    apply_events(0);
    record(0,solve(0,0,true));
    auto publish=[&]{if(stream&&!result.samples.empty()){auto samples=std::move(result.samples);Result batch=result;batch.samples=std::move(samples);stream(std::move(batch));}};
    auto next_publish=std::chrono::steady_clock::now()+std::chrono::milliseconds(80);
    publish();
    double time=0;
    size_t grid=1;
    while(time<ir.profile.stop) {
        if(paused && paused->load(std::memory_order_relaxed))publish();
        while(paused && paused->load(std::memory_order_relaxed) && !(cancel && cancel->load()))
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if(cancel && cancel->load()) { result.cancelled=true; break; }
        const double grid_time=static_cast<double>(grid)*ir.profile.step;
        double end=std::min(grid_time,ir.profile.stop);
        if(next_event<ir.events.size()) end=std::min(end,ir.events[next_event].time);
        if(end<=time) throw Diagnostic("time_resolution",ir.project_id,"Time step cannot advance floating-point time",time);
        const auto* values=&solve(end,end-time,false);
        ++result.accepted_steps;
        time=end;
        if(time==grid_time) ++grid;
        // Integrate to the edge with the old topology. Apply all simultaneous
        // gates before solving algebraic variables with continuous C/L states.
        if(apply_events(time)) values=&solve(time,0,true);
        record(time,*values);
        if(stream && result.accepted_steps%1024==0 && std::chrono::steady_clock::now()>=next_publish){publish();next_publish=std::chrono::steady_clock::now()+std::chrono::milliseconds(80);}
    }
    return result;
}
Result execute(const SimulationIR& ir,const std::atomic_bool* cancel,std::atomic<double>* simulated_time,const Recording* recording,const std::atomic_bool* paused,const std::function<void(Result&&)>& stream) {
    try{return execute_impl(ir,cancel,simulated_time,recording,paused,stream);}
    catch(Diagnostic& error) {
        if(auto origin=ir.origins.find(error.object);origin!=ir.origins.end()){error.object=origin->second.object;error.path=origin->second.instances;}
        throw;
    }
}
}
