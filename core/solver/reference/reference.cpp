#include "core/solver/reference/reference.hpp"
#include "core/solver/reference/equation_cache.hpp"
#include "core/solver/reference/adaptive.hpp"
#include "core/compiler/topology.hpp"
#include "core/model/waveform.hpp"
#include "core/model/semiconductor.hpp"
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
    for(const auto& s:ir.stamps)if(gate_controlled(s.component.kind))channels.push_back({"gate/"+s.component.id,"gate:"+s.component.name,"bool"});
    for(const auto& signal:ir.gate_signals)channels.push_back({"gate/"+signal.id,signal.name,"bool"});
    return channels;
}
void accumulate_statistics(Result& result,const Result& previous) {
    result.accepted_steps+=previous.accepted_steps;result.rejected_steps+=previous.rejected_steps;
    result.linear_solves+=previous.linear_solves;
    result.max_step_iterations=std::max(result.max_step_iterations,previous.max_step_iterations);
    result.max_scaled_residual=std::max(result.max_scaled_residual,previous.max_scaled_residual);
    if(previous.min_accepted_step>0)result.min_accepted_step=result.min_accepted_step>0?
        std::min(result.min_accepted_step,previous.min_accepted_step):previous.min_accepted_step;
    result.max_accepted_step=std::max(result.max_accepted_step,previous.max_accepted_step);
    result.max_local_error=std::max(result.max_local_error,previous.max_local_error);
    if(result.step_reduction_reason.empty())result.step_reduction_reason=previous.step_reduction_reason;
}
Result select_result(const Result& source,const std::vector<std::string>& keys){
    std::set<std::string> selected(keys.begin(),keys.end());Result result;
    result.project_id=source.project_id;result.backend=source.backend;result.precision=source.precision;result.engine=source.engine;result.profile=source.profile;
    result.accepted_steps=source.accepted_steps;result.linear_solves=source.linear_solves;result.max_step_iterations=source.max_step_iterations;result.cancelled=source.cancelled;result.max_scaled_residual=source.max_scaled_residual;result.last_time=source.last_time;
    result.snapshot=source.snapshot;
    result.rejected_steps=source.rejected_steps;result.min_accepted_step=source.min_accepted_step;
    result.max_accepted_step=source.max_accepted_step;result.max_local_error=source.max_local_error;
    result.step_reduction_reason=source.step_reduction_reason;
    std::vector<size_t> analog,gates;
    for(size_t i=0;i<source.channels.size();++i)if(selected.count(source.channels[i].object)){analog.push_back(i);result.channels.push_back(source.channels[i]);}
    for(size_t i=0;i<source.gate_objects.size();++i)if(selected.count("gate/"+source.gate_objects[i])){gates.push_back(i);result.gate_objects.push_back(source.gate_objects[i]);result.gate_names.push_back(i<source.gate_names.size()?source.gate_names[i]:std::string{});}
    if(analog.empty()&&gates.empty())return result;
    result.samples.reserve(source.samples.size());
    for(const auto& old:source.samples){Sample sample;sample.time=old.time;for(auto i:analog)sample.values.push_back(old.values[i]);for(auto i:gates)sample.gates.push_back(old.gates[i]);result.samples.push_back(std::move(sample));}
    return result;
}
static Result execute_impl(const SimulationIR& ir,const std::atomic_bool* cancel,std::atomic<double>* simulated_time,const Recording* recording,const std::atomic_bool* paused,const std::function<void(Result&&)>& stream,const ExecutionOptions* options) {
    if(ir.unknowns.empty() || !std::isfinite(ir.profile.step) || ir.profile.step<=0 || !std::isfinite(ir.profile.stop) || ir.profile.stop<=0)
        throw Diagnostic("invalid_ir",ir.project_id,"IR must have unknowns and a positive finite time profile");
    (void)method_name(ir.profile.method);
    (void)initial_state_name(ir.profile.initial_state);
    validate_step_control(ir.profile,ir.project_id);
    if(!std::isfinite(ir.profile.warmup)||ir.profile.warmup<0||ir.profile.warmup>=ir.profile.stop)
        throw Diagnostic("invalid_profile",ir.project_id,"Warm-up must be shorter than stop time");
    Result result; result.project_id=ir.project_id; result.profile=ir.profile;
    const std::set<std::string> requested=recording?std::set<std::string>(recording->channels.begin(),recording->channels.end()):std::set<std::string>();
    auto selected=[&](const std::string& key){return !recording||recording->all||requested.count(key);};
    std::vector<size_t> analog_indices,gate_indices;
    auto catalog=available_channels(ir);
    if(recording&&!recording->all)for(const auto& key:requested)
        if(std::none_of(catalog.begin(),catalog.end(),[&](const Channel& c){return c.object==key;}))throw Diagnostic("missing_recording_channel",key,"Recording channel does not exist");
    const size_t analog_count=ir.unknowns.size()+ir.observations.size();
    for(size_t i=0;i<analog_count;++i)if(selected(catalog[i].object)){analog_indices.push_back(i);result.channels.push_back(catalog[i]);}
    for(size_t i=analog_count;i<catalog.size();++i)if(selected(catalog[i].object)){gate_indices.push_back(i-analog_count);result.gate_objects.push_back(catalog[i].object.substr(5));result.gate_names.push_back(catalog[i].name);}
    std::vector<size_t> switch_indices;
    std::vector<size_t> source_indices;
    for(size_t i=0;i<ir.stamps.size();++i)
        if(ir.stamps[i].component.source.kind!=Waveform::dc)source_indices.push_back(i);
    std::vector<bool> signal_values;for(const auto& signal:ir.gate_signals)signal_values.push_back(signal.initial);
    std::vector<double> states(ir.stamps.size()), history(ir.stamps.size());
    std::vector<bool> gates(ir.stamps.size()), diode_states(ir.stamps.size()), latched(ir.stamps.size()), released(ir.stamps.size());
    std::vector<size_t> thyristor_indices;
    std::vector<size_t> diode_indices;
    for(size_t i=0;i<ir.stamps.size();++i)
        if(rectifying(ir.stamps[i].component.kind)) diode_indices.push_back(i);
    for(size_t i=0;i<ir.stamps.size();++i) {
        states[i]=ir.stamps[i].component.initial;
        if(dynamic_diode(ir.stamps[i].component))states[i]=ir.stamps[i].component.semiconductor.initial_charge;
        gates[i]=ir.stamps[i].component.closed;
        if(ir.stamps[i].component.kind==Kind::thyristor) {
            thyristor_indices.push_back(i);
            diode_states[i]=latched[i]=ir.stamps[i].component.semiconductor.initial_latched;
        }
        if(gate_controlled(ir.stamps[i].component.kind)) switch_indices.push_back(i);
    }
    if(ir.profile.initial_state==InitialState::zero) {
        std::fill(states.begin(),states.end(),0.0);
        std::fill(latched.begin(),latched.end(),false);
        std::fill(diode_states.begin(),diode_states.end(),false);
    }
    size_t next_event=0;
    const auto* resume=options?options->resume:nullptr;
    if(resume) {
        validate_snapshot(*resume,ir);
        states=resume->states;history=resume->history;gates=resume->gates;
        diode_states=resume->diodes;latched=resume->latched;signal_values=resume->signal_values;
        next_event=static_cast<size_t>(std::upper_bound(ir.events.begin(),ir.events.end(),resume->time,
            [](double time,const GateEvent& event){return time<event.time;})-ir.events.begin());
    }
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
    auto solve=[&](double t,double h,bool initialize,bool operating_point=false) -> const std::vector<double>& {
        for(auto i:thyristor_indices) {
            released[i]=false;
            if(!gates[i])diode_states[i]=latched[i];
        }
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
                solution=&equations.solve(t,h,initialize,gates,active,states,history,operating_point);
            } catch(const Diagnostic& d) {
                if(d.code!="singular_matrix")throw;
                auto detail=diagnose_singular_topology(ir,initialize,gates,active,states,t,operating_point);
                const auto& failure=detail?*detail:d;
                if(diode_indices.empty())throw failure;
                offending=failure.object;last_linear=failure.code+": "+failure.what();singular=true;
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
                const double excess_v=v-diode_threshold(stamp.component);
                const double excess_i=current-diode_threshold_current(stamp.component);
                const bool thyristor=stamp.component.kind==Kind::thyristor;
                const bool igbt=stamp.component.kind==Kind::igbt;
                const bool can_fire=(!thyristor&&!igbt)||gates[index];
                if(thyristor&&!gates[index]&&active[index]&&
                   (current<stamp.component.semiconductor.holding_current-it||excess_i<=it))
                    released[index]=true;
                const bool must_block=!gates[index]&&(igbt||(thyristor&&(!latched[index]||released[index])));
                // A newly fired parallel phase can reverse-bias the old thyristor.
                // With ideal devices the trial with both phases on is singular,
                // so an off trial must also be allowed to establish commutation.
                // Forward blocking alone cannot extinguish a latched device.
                const bool must_hold=thyristor&&!gates[index]&&latched[index]&&!released[index]&&excess_v>=-vt;
                const bool dynamic=dynamic_diode(stamp.component);
                if(dynamic)residual_v=std::max(residual_v,std::max(0.0,active[index]?-excess_v:excess_v));
                else if(active[index])residual_i=std::max(residual_i,std::max(0.0,-excess_i));
                else if(can_fire)residual_v=std::max(residual_v,std::max(0.0,excess_v));
                if((active[index]&&(must_block||(dynamic?excess_v < -vt:excess_i < -it)))||
                   (!active[index]&&(must_hold||(can_fire&&excess_v>vt)))) {
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
            message << "Semiconductor active-set solve failed after " << iterations
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
            if(s.component.kind==Kind::thyristor) {
                const double current=values[s.branch];
                const double tolerance=ir.profile.current_tolerance+ir.profile.relative_tolerance*std::abs(current);
                latched[i]=diode_states[i]&&current>=s.component.semiconductor.holding_current&&
                    current>diode_threshold_current(s.component)+tolerance;
            }
            if(dynamic_diode(s.component)) {
                const auto &model=s.component.semiconductor;
                const auto law=diode_charge_law(model);
                const double drive=law.alpha*std::max(0.0,voltage-model.forward_voltage);
                if(operating_point) states[i]=drive/law.lambda;
                else if(!initialize) {
                    const double m=h/(ir.profile.method==Method::trapezoidal?2:1);
                    const double q=(states[i]+m*(drive+(ir.profile.method==Method::trapezoidal?history[i]:0)))/(1+m*law.lambda);
                    if(!std::isfinite(q)||q<0)throw Diagnostic("invalid_charge_state",s.component.id,"Diode charge became negative or nonfinite; reduce the time step or use Backward Euler",t);
                    states[i]=q;
                }
                history[i]=operating_point?0:drive-law.lambda*states[i];
            }
        }
        return values;
    };
    auto record=[&](double t,const std::vector<double>& values) {
        result.last_time=t;
        if(simulated_time) simulated_time->store(t,std::memory_order_relaxed);
        if(t<ir.profile.warmup)return;
        // No sample or history allocation when recording is disabled.
        if(analog_indices.empty()&&gate_indices.empty())return;
        Sample sample;sample.time=t;sample.values.reserve(analog_indices.size());sample.gates.reserve(gate_indices.size());
        for(size_t index:analog_indices){
            if(index<ir.unknowns.size())sample.values.push_back(values[index]);
            else {const auto& o=ir.observations[index-ir.unknowns.size()];sample.values.push_back(o.source_stamp>=0?
                source_value(ir.stamps[o.source_stamp].component,t):
                ((o.positive<0?0:values[o.positive])-(o.negative<0?0:values[o.negative]))*o.gain+o.offset);}
        }
        for(size_t index:gate_indices)sample.gates.push_back(index<switch_indices.size()?gates[switch_indices[index]]:signal_values[index-switch_indices.size()]);
        result.samples.push_back(std::move(sample));
    };
    double time=resume?resume->time:0;
    size_t grid=resume?static_cast<size_t>(resume->next_grid):1;
    const std::vector<double>* final_values=nullptr;
    if(resume) final_values=&resume->values;
    else {
        apply_events(0);
        final_values=&solve(0,0,true,ir.profile.initial_state==InitialState::dc_operating_point);
    }
    record(time,*final_values);
    const bool adaptive=ir.profile.step_control.adaptive;
    double proposed_step=adaptive?(resume?resume->next_step:ir.profile.step):ir.profile.step;
    // Adaptive trials may replace cache entries. Keep the accepted endpoint
    // independently so cancellation after a rejected trial still has valid state.
    std::vector<double> accepted_values, before_states, before_history, coarse_values, coarse_states;
    std::vector<bool> before_diodes, before_latched, coarse_latched;
    if(adaptive){accepted_values=*final_values;final_values=&accepted_values;}
    auto publish=[&]{if(stream&&!result.samples.empty()){auto samples=std::move(result.samples);Result batch=result;batch.samples=std::move(samples);stream(std::move(batch));}};
    auto next_publish=std::chrono::steady_clock::now()+std::chrono::milliseconds(80);
    publish();
    while(time<ir.profile.stop) {
        if(options&&options->max_steps&&result.accepted_steps>=options->max_steps)break;
        if(paused && paused->load(std::memory_order_relaxed))publish();
        while(paused && paused->load(std::memory_order_relaxed) && !(cancel && cancel->load()))
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if(cancel && cancel->load()) { result.cancelled=true; break; }
        const double grid_time=static_cast<double>(grid)*ir.profile.step;
        double end=std::min(grid_time,ir.profile.stop);
        if(time<ir.profile.warmup)end=std::min(end,ir.profile.warmup);
        if(next_event<ir.events.size()) end=std::min(end,ir.events[next_event].time);
        double source_edge=std::numeric_limits<double>::infinity();
        for(auto index:source_indices)source_edge=std::min(source_edge,next_source_breakpoint(ir.stamps[index].component,time));
        end=std::min(end,source_edge);
        if(end<=time) throw Diagnostic("time_resolution",ir.project_id,"Time step cannot advance floating-point time",time);
        const std::vector<double>* values=nullptr;
        if(!adaptive) values=&solve(end,end-time,false);
        else {
            const double boundary=end;
            before_states=states;before_history=history;before_diodes=diode_states;before_latched=latched;
            auto restore=[&]{states=before_states;history=before_history;diode_states=before_diodes;latched=before_latched;};
            end=std::min(boundary,time+proposed_step);
            unsigned attempts=0;
            for(;;) {
                restore();
                if(cancel&&cancel->load()){result.cancelled=true;break;}
                const double h=end-time, middle=time+h/2;
                if(end>time&&end==boundary&&(middle<=time||middle>=end)) {
                    // Adjacent representable event/grid timestamps have no
                    // midpoint. Retain the exact boundary with one fixed step.
                    values=&solve(end,h,false);
                    break;
                }
                if(middle<=time||middle>=end)
                    throw Diagnostic("time_resolution",ir.project_id,"Adaptive half-step cannot advance floating-point time",time);
                StepError error;
                std::string failure;
                try {
                    coarse_values=solve(end,h,false);
                    coarse_states=states;coarse_latched=latched;
                    restore();
                    solve(middle,middle-time,false);
                    values=&solve(end,end-middle,false);
                    error=step_error(ir,accepted_values,coarse_values,*values,coarse_states,states);
                    if(coarse_latched!=latched&&error.ratio<2) {
                        error.ratio=2;
                        for(auto i:thyristor_indices)if(coarse_latched[i]!=latched[i]){error.object=ir.stamps[i].component.id;break;}
                    }
                } catch(const Diagnostic& diagnostic) {
                    if(diagnostic.code!="nonlinear_convergence"&&diagnostic.code!="invalid_charge_state")throw;
                    failure=diagnostic.code;error={std::numeric_limits<double>::infinity(),diagnostic.object};
                }
                const double factor=next_step_factor(ir.profile.method,error.ratio);
                if(error.ratio<=1) {
                    result.max_local_error=std::max(result.max_local_error,error.ratio);
                    proposed_step=std::clamp(h*factor,ir.profile.step_control.minimum_step,ir.profile.step);
                    break;
                }
                ++result.rejected_steps;
                result.step_reduction_reason=failure.empty()?"local_error":failure;
                const double reduced=std::max(ir.profile.step_control.minimum_step,h*std::min(.5,factor));
                if(reduced>=h||++attempts>=64) {
                    std::ostringstream message;
                    message<<"Adaptive step cannot meet tolerance at h="<<h<<" s; error/tolerance="<<error.ratio
                           <<". Reason: "<<result.step_reduction_reason<<". Check the model, minimum step and tolerances.";
                    throw Diagnostic("adaptive_step_limit",error.object.empty()?ir.project_id:error.object,message.str(),time);
                }
                end=std::min(boundary,time+reduced);
            }
            if(result.cancelled)break;
        }
        const double accepted_h=end-time;
        result.min_accepted_step=result.accepted_steps?std::min(result.min_accepted_step,accepted_h):accepted_h;
        result.max_accepted_step=std::max(result.max_accepted_step,accepted_h);
        ++result.accepted_steps;
        time=end;
        if(time==grid_time) ++grid;
        // Integrate to the edge with the old topology. Apply all simultaneous
        // gates before solving algebraic variables with continuous C/L states.
        const bool gate_event=apply_events(time);
        if(gate_event||time==source_edge) values=&solve(time,0,true);
        if(adaptive){accepted_values=*values;values=&accepted_values;}
        record(time,*values);
        final_values=values;
        if(stream && result.accepted_steps%1024==0 && std::chrono::steady_clock::now()>=next_publish){publish();next_publish=std::chrono::steady_clock::now()+std::chrono::milliseconds(80);}
    }
    if(options&&options->capture_snapshot) {
        SimulationSnapshot checkpoint;
        checkpoint.project_id=ir.project_id;checkpoint.contract=snapshot_contract(ir,time);
        checkpoint.time=time;checkpoint.next_grid=grid;
        checkpoint.next_step=adaptive?proposed_step:0;
        checkpoint.states=std::move(states);checkpoint.history=std::move(history);
        checkpoint.gates=std::move(gates);checkpoint.diodes=std::move(diode_states);
        checkpoint.latched=std::move(latched);checkpoint.signal_values=std::move(signal_values);
        checkpoint.values=*final_values;
        result.snapshot=std::move(checkpoint);
    }
    return result;
}
Result execute(const SimulationIR& ir,const std::atomic_bool* cancel,std::atomic<double>* simulated_time,const Recording* recording,const std::atomic_bool* paused,const std::function<void(Result&&)>& stream,const ExecutionOptions* options) {
    try{return execute_impl(ir,cancel,simulated_time,recording,paused,stream,options);}
    catch(Diagnostic& error) {
        if(auto origin=ir.origins.find(error.object);origin!=ir.origins.end()){error.object=origin->second.object;error.path=origin->second.instances;}
        throw;
    }
}
}
