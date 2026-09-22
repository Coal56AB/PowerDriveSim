#include "core/ir/snapshot.hpp"
#include "core/model/semiconductor.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <numeric>
#include <sstream>
namespace pds {
std::string snapshot_contract(const SimulationIR &ir, double time) {
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << std::setprecision(std::numeric_limits<double>::max_digits10);
    text << "pds-state-v1 " << ir.project_id << ' ' << ir.profile.step << ' '
         << static_cast<int>(ir.profile.method) << ' ' << ir.profile.max_iterations << ' '
         << ir.profile.voltage_tolerance << ' ' << ir.profile.current_tolerance << ' '
         << ir.profile.relative_tolerance << ' ' << ir.node_count << '\n';
    if (ir.profile.step_control.adaptive) {
        const auto &control = ir.profile.step_control;
        text << "adaptive " << control.minimum_step << ' ' << control.relative_tolerance << ' '
             << control.voltage_tolerance << ' ' << control.current_tolerance << ' ' << control.charge_tolerance << '\n';
    }
    for (const auto &channel : ir.unknowns)
        text << channel.object << '\n';
    for (const auto &stamp : ir.stamps) {
        const auto &c = stamp.component;
        const auto &s = c.source;
        const auto &d = c.semiconductor;
        text << c.id << ' ' << static_cast<int>(c.kind) << ' ' << stamp.positive << ' ' << stamp.negative
             << ' ' << stamp.branch << ' ' << c.value << ' ' << c.initial << ' ' << c.closed << ' '
             << static_cast<int>(s.kind) << ' ' << s.offset << ' ' << s.frequency << ' ' << s.phase << ' '
             << s.delay << ' ' << s.duty << ' ' << s.points.size();
        for (const auto &point : s.points)
            text << ' ' << point.x << ' ' << point.y;
        text << ' ' << static_cast<int>(d.model) << ' ' << d.ron << ' ' << d.roff << ' ' << d.forward_voltage
             << ' ' << d.charge_dynamics << ' ' << d.transit_time << ' ' << d.carrier_lifetime << ' '
             << d.initial_charge << ' ' << d.holding_current << ' ' << d.initial_latched << '\n';
    }
    for (const auto &signal : ir.gate_signals)
        text << signal.id << ' ' << signal.initial << '\n';
    for (const auto &program : ir.gate_programs) {
        text << "gate-program " << program.id << ' ' << program.source.size() << ' ' << program.source;
        for (const auto &output : program.outputs) {
            text << " output " << output.signal;
            for (const auto target : output.targets) text << ' ' << target;
        }
        text << '\n';
    }
    for(const auto &task:ir.signal.tasks) {
        text<<"signal-task "<<task.id<<' '<<task.period<<' '<<task.phase<<' '
            <<task.code.size()<<' '<<task.code<<' '<<task.inputs.size()<<' '<<task.outputs.size()<<'\n';
        for(const auto &input:task.inputs)
            text<<"signal-input "<<input.port.id<<' '<<input.port.name<<' '
                <<static_cast<unsigned>(input.port.type)<<' '<<input.port.unit.size()<<' '
                <<input.port.unit<<' '<<input.port.initial<<' '
                <<input.source.object<<' '<<input.source.port<<'\n';
        for(const auto &output:task.outputs)
            text<<"signal-output "<<output.id<<' '<<output.name<<' '
                <<static_cast<unsigned>(output.type)<<' '<<output.unit.size()<<' '
                <<output.unit<<' '<<output.initial<<'\n';
    }
    for(const auto &binding:ir.signal_inputs)
        text<<"signal-binding "<<binding.endpoint.object<<' '<<binding.endpoint.port<<' '
            <<static_cast<unsigned>(binding.type)<<' '<<binding.unit.size()<<' '
            <<binding.unit<<' '<<static_cast<unsigned>(binding.source)<<' '<<binding.index<<'\n';
    for(const auto &binding:ir.signal_gates) {
        text<<"signal-gate "<<binding.endpoint.object<<' '<<binding.endpoint.port;
        for(const auto target:binding.targets)text<<' '<<target;
        text<<'\n';
    }
    for (const auto &event : ir.events) {
        if (event.time > time)
            break;
        text << event.target << ' ' << event.time << ' ' << event.closed << '\n';
    }
    return derived_uuid(text.str());
}
void validate_snapshot(const SimulationSnapshot &s, const SimulationIR &ir) {
    const auto invalid = [&](const char *message) {
        throw Diagnostic("invalid_snapshot", ir.project_id, message);
    };
    if ((s.version < 1 || s.version > 4) || !std::isfinite(s.time) || s.time < 0 || s.time > ir.profile.stop ||
        s.next_grid == 0)
        invalid("Snapshot version or time is invalid for this run");
    if (s.project_id != ir.project_id || s.contract != snapshot_contract(ir, s.time))
        invalid("Snapshot belongs to a different model, integration profile or past gate schedule");
    if (!std::isfinite(s.next_step) || (ir.profile.step_control.adaptive
        ? s.version < 2 || s.next_step < ir.profile.step_control.minimum_step || s.next_step > ir.profile.step
        : s.next_step != 0))
        invalid("Snapshot adaptive step is invalid");
    const auto count = ir.stamps.size();
    if (s.states.size() != count || s.history.size() != count || s.gates.size() != count ||
        s.diodes.size() != count || s.latched.size() != count || s.values.size() != ir.unknowns.size() ||
        s.signal_values.size() != ir.gate_signals.size() ||
        (ir.gate_programs.empty() ? !s.gate_program_states.empty()
                                  : s.version < 3 || s.gate_program_states.size() != ir.gate_programs.size()))
        invalid("Snapshot arrays do not match the model");
    for (const auto *values : {&s.states, &s.history, &s.values})
        if (std::any_of(values->begin(), values->end(), [](double v) { return !std::isfinite(v); }))
            invalid("Snapshot contains a nonfinite value");
    for(const auto &program:s.gate_program_states)
        for(const auto &[key,value]:program.static_values) {
            (void)key;
            if(!std::isfinite(value))invalid("Snapshot contains a nonfinite Gate C state");
        }
    if(ir.signal.tasks.empty()) {
        if(!s.signal_tasks.empty()||!s.signal_outputs.empty()||!s.accepted_signal_inputs.empty())
            invalid("Snapshot contains unexpected signal state");
    } else {
        if(s.version<4||s.signal_tasks.size()!=ir.signal.tasks.size())
            invalid("Snapshot signal tasks do not match the model");
        for(const auto &task:ir.signal.tasks) {
            const auto saved=s.signal_tasks.find(task.id);
            if(saved==s.signal_tasks.end()||saved->second.outputs.size()!=task.outputs.size())
                invalid("Snapshot signal task is missing or has different outputs");
            for(const auto &output:task.outputs) {
                const auto value=saved->second.outputs.find(output.name);
                const auto frame=s.signal_outputs.find(signal_endpoint_key({task.id,output.id}));
                if(value==saved->second.outputs.end()||!std::isfinite(value->second)||
                   (output.type==SignalScalarType::boolean&&value->second!=0&&value->second!=1)||
                   frame==s.signal_outputs.end()||frame->second.type!=output.type||
                   frame->second.unit!=output.unit||frame->second.value!=value->second)
                    invalid("Snapshot signal output does not match the task");
            }
            for(const auto &[key,value]:saved->second.program_state.static_values) {
                (void)key;
                if(!std::isfinite(value))invalid("Snapshot contains a nonfinite code-block state");
            }
        }
        const auto output_count=std::accumulate(ir.signal.tasks.begin(),ir.signal.tasks.end(),size_t{},
            [](size_t count,const SignalTaskIR &task){return count+task.outputs.size();});
        if(s.signal_outputs.size()!=output_count)
            invalid("Snapshot signal frame has unexpected outputs");
        for(const auto &[key,value]:s.signal_outputs) {
            (void)key;
            if(!std::isfinite(value.value)||!std::isfinite(value.time)||value.time<0||
               value.time>s.time+std::max(1.0,std::abs(s.time))*1e-13||
               (value.type==SignalScalarType::boolean&&value.value!=0&&value.value!=1))
                invalid("Snapshot signal frame is invalid");
        }
        for(const auto &[key,value]:s.accepted_signal_inputs) {
            const auto binding=std::find_if(ir.signal_inputs.begin(),ir.signal_inputs.end(),
                [&](const SignalInputBinding &candidate){return signal_endpoint_key(candidate.endpoint)==key;});
            if(binding==ir.signal_inputs.end()||value.type!=binding->type||value.unit!=binding->unit||
               !std::isfinite(value.value)||!std::isfinite(value.time)||value.time<0||
               value.time>s.time+std::max(1.0,std::abs(s.time))*1e-13||
               (value.type==SignalScalarType::boolean&&value.value!=0&&value.value!=1))
                invalid("Snapshot accepted signal frame is invalid");
        }
    }
    const double next = static_cast<double>(s.next_grid) * ir.profile.step;
    const double previous = static_cast<double>(s.next_grid - 1) * ir.profile.step;
    if (!std::isfinite(next) || next <= s.time || previous > s.time)
        invalid("Snapshot grid position does not match its time");
    for (size_t k = 0; k < count; ++k) {
        const auto &stamp = ir.stamps[k];
        const auto voltage = (stamp.positive < 0 ? 0 : s.values.at(stamp.positive)) -
                             (stamp.negative < 0 ? 0 : s.values.at(stamp.negative));
        if (stamp.component.kind == Kind::capacitor &&
            (s.states[k] != voltage || s.history[k] != s.values.at(stamp.branch)))
            invalid("Snapshot capacitor state and algebraic values disagree");
        if (stamp.component.kind == Kind::inductor &&
            (s.states[k] != s.values.at(stamp.branch) || s.history[k] != voltage))
            invalid("Snapshot inductor state and algebraic values disagree");
        if (dynamic_diode(stamp.component) && s.states[k] < 0)
            invalid("Snapshot diode charge is negative");
    }
}
} // namespace pds
