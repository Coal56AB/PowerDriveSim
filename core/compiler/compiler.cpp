#include "core/ir/ir.hpp"
#include "core/compiler/topology.hpp"
#include "core/model/waveform.hpp"
#include "core/model/semiconductor.hpp"
#include "core/model/connectivity.hpp"
#include "core/model/expression.hpp"
#include "core/model/c_program.hpp"
#include "core/model/hierarchy.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
namespace pds {
namespace {
std::vector<std::string> split_arguments(const std::string &text) {
    std::vector<std::string> result;
    size_t start = 0;
    int depth = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || (text[i] == ',' && depth == 0)) {
            result.push_back(text.substr(start, i - start));
            start = i + 1;
        } else if (text[i] == '(') {
            ++depth;
        } else if (text[i] == ')') {
            --depth;
            if (depth < 0)
                throw Diagnostic("invalid_gate_script", "", "Unbalanced parentheses");
        }
    }
    if (depth != 0)
        throw Diagnostic("invalid_gate_script", "", "Unbalanced parentheses");
    return result;
}
using ScriptProgram = ExpressionProgram;
ExpressionOptions gate_expression_options(const std::string &object = {}) {
    return {"invalid_gate_script", object, true, true};
}
ScriptProgram script_program(const std::string &code) {
    return parse_expression_program(code, gate_expression_options());
}
double script_number(const std::string &token, double t,
                     const std::map<std::string, std::string> &variables = {}) {
    return evaluate_expression(token, variables, t, gate_expression_options());
}
std::vector<double> script_arguments(const std::string &code, const std::string &name, std::optional<double> t = {},
                                     const std::map<std::string, std::string> &variables = {}) {
    if (code.rfind(name + "(", 0) != 0 || code.back() != ')')
        return {};
    std::vector<double> result;
    for (const auto &token : split_arguments(code.substr(name.size() + 1, code.size() - name.size() - 2))) {
        if (token.empty())
            throw Diagnostic("invalid_gate_script", "", "Gate script argument is empty");
        if (!t) {
            if (expression_depends_on_time(token, variables))
                return {};
        }
        result.push_back(script_number(token, t.value_or(0), variables));
    }
    return result;
}
std::vector<double> script_pwm_arguments(const GatePattern &g, std::optional<double> t = {},
                                         const ScriptProgram *prepared = nullptr) {
    const auto parsed = prepared ? ScriptProgram{} : script_program(g.code);
    const auto &variables = prepared ? prepared->variables : parsed.variables;
    const auto &expression = prepared ? prepared->expression : parsed.expression;
    for (const auto *name : {"pwm", "square"}) {
        auto args = script_arguments(expression, name, t, variables);
        if (!args.empty()) {
            if (args.size() != 3 || args[0] <= 0 || args[1] < 0 || args[1] > 1 || args[2] < 0)
                throw Diagnostic("invalid_gate_script", g.id, "Use pwm(frequency,duty,delay) with duty 0..1");
            return args;
        }
    }
    return {};
}
std::optional<std::array<double, 4>> ramp_arguments(std::string token,
                                                    const std::map<std::string, std::string> &variables = {}) {
    if (auto variable = variables.find(token); variable != variables.end())
        token = variable->second;
    if (token.rfind("ramp(", 0) != 0 || token.back() != ')')
        return {};
    const auto args = split_arguments(token.substr(5, token.size() - 6));
    if (args.size() != 4)
        throw Diagnostic("invalid_gate_script", "", "Use ramp(t0,t1,value0,value1)");
    std::array<double, 4> values{};
    for (size_t i = 0; i < args.size(); ++i) {
        if (expression_depends_on_time(args[i], variables))
            return {};
        values[i] = script_number(args[i], 0, variables);
    }
    if (values[1] <= values[0])
        throw Diagnostic("invalid_gate_script", "", "Ramp end time must be greater than start time");
    return values;
}
bool generate_phase_pwm_edges(const GatePattern &g, double stop,
                              const std::function<void(double, bool)> &edge,
                              const ScriptProgram *prepared = nullptr,
                              size_t max_edges = std::numeric_limits<size_t>::max()) {
    const auto parsed = prepared ? ScriptProgram{} : script_program(g.code);
    const auto &variables = prepared ? prepared->variables : parsed.variables;
    const auto &expression = prepared ? prepared->expression : parsed.expression;
    if (expression.rfind("phasepwm(", 0) != 0 || expression.back() != ')')
        return false;
    const auto args = split_arguments(expression.substr(9, expression.size() - 10));
    if (args.size() != 3)
        throw Diagnostic("invalid_gate_script", g.id, "Use phasepwm(frequency,duty,delay) with duty 0..1");
    if (expression_depends_on_time(args[0], variables) ||
        expression_depends_on_time(args[1], variables))
        return false;
    const double frequency = script_number(args[0], 0, variables);
    const double duty = script_number(args[1], 0, variables);
    if (frequency <= 0 || duty < 0 || duty > 1)
        throw Diagnostic("invalid_gate_script", g.id, "Use phasepwm(frequency,duty,delay) with duty 0..1");
    if (duty == 0 || duty == 1)
        return true;
    if (stop > 0 && frequency > static_cast<double>(max_edges) / (2.0 * stop))
        return false;
    const double period = 1.0 / frequency;
    auto emit_interval = [&](double begin, double end, double slope, double intercept) {
        if (end <= begin)
            return;
        const double factor = 1.0 - slope;
        if (std::abs(factor) < 1e-12)
            return;
        const double phase_begin = factor * begin - intercept;
        const double phase_end = factor * end - intercept;
        const double low = std::min(phase_begin, phase_end) - period;
        const double high = std::max(phase_begin, phase_end) + period;
        const long long first = static_cast<long long>(std::floor(low / period)) - 1;
        const long long last = static_cast<long long>(std::ceil(high / period)) + 1;
        for (long long n = first; n <= last; ++n) {
            for (auto [offset, state] : {std::pair{0.0, true}, std::pair{duty * period, false}}) {
                const double t = (n * period + offset + intercept) / factor;
                if (t > begin + 1e-12 && t <= end + 1e-12 && t > 0)
                    edge(std::clamp(t, begin, end), state);
            }
        }
    };
    const auto ramp = ramp_arguments(args[2], variables);
    if (!ramp) {
        if (expression_depends_on_time(args[2], variables))
            return false;
        emit_interval(0, stop, 0, script_number(args[2], 0, variables));
        return true;
    }
    const auto [t0, t1, d0, d1] = *ramp;
    emit_interval(0, std::min(stop, t0), 0, d0);
    const double slope = (d1 - d0) / (t1 - t0);
    emit_interval(std::max(0.0, t0), std::min(stop, t1), slope, d0 - slope * t0);
    emit_interval(std::max(0.0, t1), stop, 0, d1);
    return true;
}
bool gate_script_value(const GatePattern &g, double t, const ScriptProgram *prepared = nullptr) {
    const auto parsed = prepared ? ScriptProgram{} : script_program(g.code);
    const auto &variables = prepared ? prepared->variables : parsed.variables;
    const auto &expression = prepared ? prepared->expression : parsed.expression;
    if (expression == "1" || expression == "true") return true;
    if (expression == "0" || expression == "false") return false;
    if (auto args = script_arguments(expression, "phasepwm", t, variables); !args.empty()) {
        if (args.size() != 3 || args[0] <= 0 || args[1] < 0 || args[1] > 1)
            throw Diagnostic("invalid_gate_script", g.id, "Use phasepwm(frequency,duty,delay) with duty 0..1");
        if (args[1] == 0)
            return false;
        if (args[1] == 1)
            return true;
        const double period = 1.0 / args[0];
        double delay = std::fmod(args[2], period);
        if (delay < 0)
            delay += period;
        double phase = std::fmod(t - delay, period);
        if (phase < 0)
            phase += period;
        const double boundary = args[1] * period;
        const double tolerance = 64 * std::numeric_limits<double>::epsilon() *
                                 std::max({period, std::abs(t), std::abs(delay)});
        return phase < boundary - tolerance;
    }
    if (auto args = script_pwm_arguments(g, t, prepared ? prepared : &parsed); !args.empty()) {
            if (t < args[2] || args[1] == 0) return false;
            if (args[1] == 1) return true;
            const double phase = std::fmod((t - args[2]) * args[0], 1.0);
            return phase >= 0 && phase < args[1];
    }
    return script_number(expression, t, variables) != 0;
}
std::optional<std::string> simple_return_expression(const std::string &code) {
    const auto first = code.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || code.compare(first, 6, "return") != 0)
        return {};
    const auto after_keyword = first + 6;
    if (after_keyword < code.size() &&
        (std::isalnum(static_cast<unsigned char>(code[after_keyword])) || code[after_keyword] == '_'))
        return {};
    const auto semicolon = code.find(';', after_keyword);
    if (semicolon == std::string::npos || code.find(';', semicolon + 1) != std::string::npos ||
        code.find_first_not_of(" \t\r\n", semicolon + 1) != std::string::npos)
        return {};
    const auto expression_begin = code.find_first_not_of(" \t\r\n", after_keyword);
    if (expression_begin == std::string::npos || expression_begin >= semicolon)
        return {};
    const auto expression_end = code.find_last_not_of(" \t\r\n", semicolon - 1);
    if (expression_end == std::string::npos || expression_end < expression_begin)
        return {};
    return code.substr(expression_begin, expression_end - expression_begin + 1);
}
bool gate_c_value(const CProgram &program,CProgramState &state,double time) {
    const auto result=execute_c_program(program,time,&state);
    return result.return_value.value_or(0)!=0;
}
std::string gate_port(unsigned index) { return index==0?"out":"out"+std::to_string(index); }
std::string gate_signal_id(const GatePattern& gate,unsigned index) {
    return index==0?gate.id:derived_uuid("gate-output:"+gate.id+":"+std::to_string(index));
}
} // namespace

static SimulationIR compile_flat(const Project& p) {
    if(p.schema!=project_schema) throw Diagnostic("schema_version",p.id,"Unsupported schema");
    std::set<std::string> ids;
    auto check_id=[&](const std::string& id) {
        if(!valid_uuid(id) || !ids.insert(id).second) throw Diagnostic("invalid_uuid",id,"UUID is invalid or duplicated");
    };
    check_id(p.id);
    if(!std::isfinite(p.profile.stop) || !std::isfinite(p.profile.step) || p.profile.stop<=0 || p.profile.step<=0)
        throw Diagnostic("invalid_profile",p.id,"Stop time and step must be positive finite SI values");
    (void)method_name(p.profile.method);
    (void)initial_state_name(p.profile.initial_state);
    if (!std::isfinite(p.profile.warmup) || p.profile.warmup < 0 || p.profile.warmup >= p.profile.stop)
        throw Diagnostic("invalid_profile", p.id, "Warm-up must be finite, nonnegative and shorter than stop time");
    validate_step_control(p.profile, p.id);
    if(p.profile.max_iterations<1 || p.profile.max_iterations>1024
       || !std::isfinite(p.profile.voltage_tolerance) || p.profile.voltage_tolerance<=0
       || !std::isfinite(p.profile.current_tolerance) || p.profile.current_tolerance<=0
       || !std::isfinite(p.profile.relative_tolerance) || p.profile.relative_tolerance<0)
        throw Diagnostic("invalid_profile",p.id,"Nonlinear tolerances must be finite/positive; iteration budget must be 1..1024");
    SimulationIR ir; ir.project_id=p.id; ir.profile=p.profile;
    auto nodes=p.nodes; auto components=p.components;
    std::sort(nodes.begin(),nodes.end(),[](const Node& a,const Node& b){return a.id<b.id;});
    std::sort(components.begin(),components.end(),[](const Component& a,const Component& b){return a.id<b.id;});
    std::map<std::string,int> indices;
    std::set<std::string> reached;
    for(const auto& n:nodes) {
        check_id(n.id);
        indices[n.id]=n.ground?-1:ir.node_count++;
        if(n.ground) {reached.insert(n.id);ir.observations.push_back({{n.id,"u:"+n.name,"V"},-1,-1});}
        else ir.unknowns.push_back({n.id,"u:"+n.name,"V"});
    }
    if(reached.empty()) throw Diagnostic("missing_ground",p.id,"Add an electrical reference node");
    for(const auto& c:components) {
        check_id(c.id);
        (void)kind_name(c.kind);
        validate_waveform(c);
        validate_semiconductor(c);
        if(!indices.count(c.positive) || !indices.count(c.negative))
            throw Diagnostic("missing_terminal",c.id,"Connect both terminals to existing electrical nodes");
        if(c.positive==c.negative && c.kind!=Kind::voltage_probe) throw Diagnostic("shorted_component",c.id,"Both terminals reference the same node");
        if(!std::isfinite(c.value) || !std::isfinite(c.initial) || !std::isfinite(c.x) || !std::isfinite(c.y))
            throw Diagnostic("invalid_parameter",c.id,"Parameters must be finite");
        if((c.kind==Kind::resistor || c.kind==Kind::capacitor || c.kind==Kind::inductor) && c.value<=0)
            throw Diagnostic("invalid_parameter",c.id,"R, L and C must be strictly positive");
        if(c.parallel_resistance_enabled) {
            if(c.kind!=Kind::inductor)
                throw Diagnostic("invalid_parameter",c.id,"Parallel resistance is supported only for inductors");
            if(!std::isfinite(c.parallel_resistance)||c.parallel_resistance<=0)
                throw Diagnostic("invalid_parameter",c.id,"Parallel resistance must be strictly positive");
        }
        if(c.kind==Kind::diode && (c.value!=0 || c.initial!=0 || c.closed))
            throw Diagnostic("invalid_parameter",c.id,"Ideal diode uses value=0, initial=0 and closed=0; its state is solved automatically");
        if((c.kind==Kind::thyristor || c.kind==Kind::igbt) && (c.value!=0 || c.initial!=0))
            throw Diagnostic("invalid_parameter",c.id,"Controlled rectifier uses value=0 and initial=0; thyristor latch parameters are explicit");
        Stamp s{c,indices.at(c.positive),indices.at(c.negative),-1};
        if(c.kind!=Kind::resistor && c.kind!=Kind::current && c.kind!=Kind::voltage_probe) {
            s.branch=static_cast<int>(ir.unknowns.size());
            ir.unknowns.push_back({c.id,"i:"+c.name,"A"});
        }
        if(c.kind==Kind::voltage_probe)
            ir.observations.push_back({{c.id,"u:"+c.name,"V"},s.positive,s.negative});
        else ir.stamps.push_back(s);
        if(c.kind==Kind::resistor)ir.observations.push_back({{c.id,"i:"+c.name,"A"},s.positive,s.negative,1/c.value,0});
        if(c.kind==Kind::current)ir.observations.push_back({{c.id,"i:"+c.name,"A"},-1,-1,0,c.value,
            c.source.kind==Waveform::dc?-1:static_cast<int>(ir.stamps.size()-1)});
    }
    // Current sources do not establish a voltage-reference path.
    // State-dependent ideal loops/islands are diagnosed after a failed solve.
    for(size_t pass=0;pass<nodes.size();++pass)
        for(const auto& c:components) if(c.kind!=Kind::current && c.kind!=Kind::voltage_probe) {
            if(reached.count(c.positive)) reached.insert(c.negative);
            if(reached.count(c.negative)) reached.insert(c.positive);
        }
    for(const auto& n:nodes) if(!reached.count(n.id))
        throw Diagnostic("floating_node",n.id,"No structural voltage-reference path; connect an explicit reference path");
    validate_source_loops(ir);
    ir.events=p.events;
    // Validate timestamps before sorting: NaN violates strict weak ordering.
    for(const auto& e:ir.events)
        if(!std::isfinite(e.time) || e.time<0 || e.time>p.profile.stop)
            throw Diagnostic("invalid_event",e.target,"Event time must lie in the simulation interval");
    std::sort(ir.events.begin(),ir.events.end(),[](const GateEvent& a,const GateEvent& b){
        return a.time==b.time?a.target<b.target:a.time<b.time;
    });
    for(size_t i=0;i<ir.events.size();++i) {
        const auto& e=ir.events[i];
        if(!std::isfinite(e.time) || e.time<0 || e.time>p.profile.stop)
            throw Diagnostic("invalid_event",e.target,"Event time must lie in the simulation interval");
        auto it=std::find_if(components.begin(),components.end(),[&](const Component& c){return c.id==e.target;});
        if(it==components.end() || !gate_controlled(it->kind))
            throw Diagnostic("invalid_gate_target",e.target,"Gate events require a gate-controlled device");
        if(i && ir.events[i-1].time==e.time && ir.events[i-1].target==e.target)
            throw Diagnostic("conflicting_gate_events",e.target,"Only one gate assignment per switch and timestamp is allowed");
    }
    std::set<std::pair<int,int>> pattern;
    for(const auto& s:ir.stamps)
        for(int a:{s.positive,s.negative,s.branch}) for(int b:{s.positive,s.negative,s.branch})
            if(a>=0 && b>=0) pattern.emplace(a,b);
    ir.sparsity.assign(pattern.begin(),pattern.end());
    return ir;
}
static SimulationIR compile_wired(const Project& source, const std::map<std::string,ObjectPath>& origins) {
    Project project=source;
    constexpr size_t max_optimized_script_edges = 200000;
    std::set<std::string> runtime_gate_scripts;
    // Manual table edges remain in the document while another gate mode is
    // active. They are inactive input, not a second driver of the same signal.
    for(const auto& g:project.patterns)if(g.pwm||g.script)
        std::erase_if(project.events,[&](const GateEvent& event){return event.target==g.id;});
    (void)resolve_connections(project); // Validate active parameters before generating scheduled edges.
    if(!std::isfinite(project.profile.stop)||project.profile.stop<=0)throw Diagnostic("invalid_profile",project.id,"Stop time must be positive and finite");
    size_t generated=0;
    auto generated_edge = [&](const GatePattern &g, double time, bool state, size_t &generated) {
        const double tolerance = 64 * std::numeric_limits<double>::epsilon() *
                                 std::max({1.0, std::abs(time), std::abs(project.profile.stop)});
        if(time>0&&time<=project.profile.stop+tolerance){
            if(time>project.profile.stop)time=project.profile.stop;
            if(++generated>1000000)throw Diagnostic("pwm_event_limit",g.id,"Generated gate signal exceeds one million edges; reduce frequency, script step or duration");
            project.events.push_back({time,g.id,state});
        }
    };
    for(auto& g:project.patterns)if(g.pwm){
        g.initial=g.delay==0&&g.duty>0;
        if(g.duty==0||g.delay>project.profile.stop)continue;
        auto edge=[&](double time,bool state){generated_edge(g,time,state,generated);};
        if(g.duty==1){edge(g.delay,true);continue;}
        double count=(project.profile.stop-g.delay)*g.frequency;
        if(!std::isfinite(count)||count>500000)throw Diagnostic("pwm_event_limit",g.id,"PWM exceeds one million edges; reduce frequency or simulation duration");
        for(size_t k=0;k<=static_cast<size_t>(std::floor(count));++k){double rise=g.delay+static_cast<double>(k)/g.frequency;double fall=g.delay+(static_cast<double>(k)+g.duty)/g.frequency;edge(rise,true);edge(fall,false);}
    }
    for(auto& g:project.patterns)if(g.script){
      try {
        CProgramOptions c_options;c_options.diagnostic_code="invalid_gate_script";c_options.object=g.id;
        c_options.allow_time=true;c_options.allow_gate_functions=true;c_options.require_return=g.outputs==1;
        if(g.outputs>1){c_options.external_arrays["IN"]=g.outputs;c_options.writable_arrays.insert("IN");}
        const auto c_program=compile_c_program(g.code,c_options);
        auto optimized = g;
        std::optional<ScriptProgram> optimization_program;
        try {
            if(g.outputs>1)throw Diagnostic("not_scalar_gate_optimizer",g.id,"Multiple outputs use the safe C runtime");
            if (const auto simple_expression = simple_return_expression(g.code))
                optimized.code = *simple_expression;
            optimization_program = script_program(optimized.code);
        } catch (const Diagnostic &) {
            // Full safe C remains valid even when it is intentionally outside
            // the much smaller, side-effect-free expression optimizer.
        }
        if(optimization_program) {
            const auto &program=*optimization_program;
            {
                std::vector<GateEvent> candidate_events;
                const auto remaining = generated < max_optimized_script_edges
                    ? max_optimized_script_edges - generated : 0;
                bool overflow = false;
                auto edge=[&](double time,bool state) {
                    if(candidate_events.size() >= remaining) {
                        overflow = true;
                        return;
                    }
                    const double tolerance = 64 * std::numeric_limits<double>::epsilon() *
                                             std::max({1.0, std::abs(time), std::abs(project.profile.stop)});
                    if(time>0&&time<=project.profile.stop+tolerance) {
                        if(time>project.profile.stop)time=project.profile.stop;
                        candidate_events.push_back({time,g.id,state});
                    }
                };
                if(generate_phase_pwm_edges(optimized,project.profile.stop,edge,&program,remaining) && !overflow) {
                    g.initial=gate_script_value(optimized,0,&program);
                    generated += candidate_events.size();
                    project.events.insert(project.events.end(), candidate_events.begin(), candidate_events.end());
                    continue;
                }
            }
            if(auto args=script_pwm_arguments(optimized,{},&program);!args.empty()) {
                g.initial=args[2]==0&&args[1]>0;
                if(args[1]==0||args[2]>project.profile.stop)continue;
                if(args[1]==1){generated_edge(g,args[2],true,generated);continue;}
                double count=(project.profile.stop-args[2])*args[0];
                if(std::isfinite(count) && count<=100000 &&
                   generated + 2 * (static_cast<size_t>(std::floor(count)) + 1) <= max_optimized_script_edges) {
                    auto edge=[&](double time,bool state){generated_edge(g,time,state,generated);};
                    for(size_t k=0;k<=static_cast<size_t>(std::floor(count));++k){double rise=args[2]+static_cast<double>(k)/args[0];double fall=args[2]+(static_cast<double>(k)+args[1])/args[0];edge(rise,true);edge(fall,false);}
                    continue;
                }
            }
        }
        if(g.outputs==1) {
            CProgramState c_state;
            g.initial=gate_c_value(c_program,c_state,0);
        }
        runtime_gate_scripts.insert(g.id);
      } catch(const Diagnostic& diagnostic) {
        if(diagnostic.code=="invalid_gate_script"&&diagnostic.object.empty())
            throw Diagnostic(diagnostic.code,g.id,diagnostic.what(),diagnostic.time);
        throw;
      }
    }

    std::sort(project.events.begin(), project.events.end(), [](const GateEvent &a, const GateEvent &b) {
        return a.time == b.time ? a.target < b.target : a.time < b.time;
    });
    double simultaneous_time = 0;
    bool have_time = false;
    for (auto &event : project.events) {
        const double scale = std::max({1.0, std::abs(simultaneous_time), std::abs(event.time)});
        if (have_time && std::abs(event.time - simultaneous_time) <=
                             64 * std::numeric_limits<double>::epsilon() * scale) {
            event.time = simultaneous_time;
        } else {
            simultaneous_time = event.time;
            have_time = true;
        }
    }

    auto resolved=resolve_connections(project,origins);
    auto ir=compile_flat(resolved.project);
    auto patterns=project.patterns;std::sort(patterns.begin(),patterns.end(),[](const GatePattern& a,const GatePattern& b){return a.id<b.id;});
    for(const auto& pattern:patterns)for(unsigned index=0;index<pattern.outputs;++index)
        ir.gate_signals.push_back({gate_signal_id(pattern,index),pattern.name+(pattern.outputs>1?"["+std::to_string(index)+"]":""),pattern.initial});
    for(const auto& pattern:patterns)if(runtime_gate_scripts.count(pattern.id)) {
        GateProgram program{pattern.id,pattern.code,{}};
        for(unsigned index=0;index<pattern.outputs;++index) {
            const auto signal_id=gate_signal_id(pattern,index);
            const auto signal=std::find_if(ir.gate_signals.begin(),ir.gate_signals.end(),[&](const GateSignal& candidate){return candidate.id==signal_id;});
            GateProgramOutput output{static_cast<size_t>(signal-ir.gate_signals.begin()),{}};
            const auto driver=endpoint_key({pattern.id,gate_port(index)});
            for(const auto& [target,connected]:resolved.gate_drivers)if(connected==driver) {
                const auto stamp=std::find_if(ir.stamps.begin(),ir.stamps.end(),[&](const Stamp& candidate){return candidate.component.id==target;});
                if(stamp!=ir.stamps.end())output.targets.push_back(static_cast<size_t>(stamp-ir.stamps.begin()));
            }
            program.outputs.push_back(std::move(output));
        }
        ir.gate_programs.push_back(std::move(program));
    }
    for(const auto& event:project.events)
        if(std::any_of(project.patterns.begin(),project.patterns.end(),[&](const GatePattern& p){return p.id==event.target;}))ir.events.push_back(event);
    std::sort(ir.events.begin(),ir.events.end(),[](const GateEvent& a,const GateEvent& b){return a.time==b.time?a.target<b.target:a.time<b.time;});
    return ir;
}

SimulationIR compile(const Project& source) {
    Project active=source;
    auto select_gate_modes=[](Schematic& schematic) {
        for(const auto& gate:schematic.patterns)if(gate.pwm||gate.script)
            std::erase_if(schematic.events,[&](const GateEvent& event){return event.target==gate.id;});
    };
    select_gate_modes(active);
    for(auto& definition:active.definitions)select_gate_modes(definition);
    auto resolved=resolve_parameter_expressions(active);
    auto expanded=flatten(resolved);
    try {
        if(!active.instances.empty()) {
            for(const auto& [terminal,net]:resolve_connections(expanded.project).nets) {
                auto origin=expanded.origins.find(terminal.substr(0,terminal.find('/')));
                if(origin!=expanded.origins.end())expanded.origins.try_emplace(net,origin->second);
            }
        }
        auto ir=compile_wired(expanded.project,expanded.origins);
        ir.origins=std::move(expanded.origins);
        return ir;
    } catch(Diagnostic& error) {
        if(auto origin=expanded.origins.find(error.object);origin!=expanded.origins.end()) {
            error.object=origin->second.object;error.path=origin->second.instances;
        }
        throw;
    }
}

}
