#include "results/csv.hpp"
#include <iomanip>
#include <ostream>
namespace pds {
static std::string quote(const std::string& s) {
    std::string v="\"";
    for(char c:s) { if(c=='"') v+='"'; v+=c; }
    return v+'"';
}
void write_csv(const Result& r,std::ostream& out) {
    out << "# PowerDriveSim " << r.engine << "; project=" << r.project_id << "; backend=" << r.backend
        << "; precision=" << r.precision << "; method=" << method_name(r.profile.method) << "; step=" << std::setprecision(17) << r.profile.step
        << "; nonlinear_max_iterations=" << r.profile.max_iterations
        << "; voltage_tolerance=" << r.profile.voltage_tolerance << "; current_tolerance=" << r.profile.current_tolerance
        << "; relative_tolerance=" << r.profile.relative_tolerance << "; linear_solves=" << r.linear_solves
        << "; stop=" << r.profile.stop << "; cancelled=" << r.cancelled
        << "; initial_state=" << initial_state_name(r.profile.initial_state) << "; warmup=" << r.profile.warmup
        << "; adaptive=" << r.profile.step_control.adaptive << "; accepted=" << r.accepted_steps
        << "; rejected=" << r.rejected_steps << "; min_accepted_step=" << r.min_accepted_step
        << "; max_accepted_step=" << r.max_accepted_step;
    if(r.profile.step_control.adaptive) {
        const auto &c=r.profile.step_control;
        out<<"; minimum_step="<<c.minimum_step<<"; step_rtol="<<c.relative_tolerance
           <<"; step_atol_V="<<c.voltage_tolerance<<"; step_atol_A="<<c.current_tolerance
           <<"; step_atol_C="<<c.charge_tolerance;
    }
    out<<"\ntime[s]";
    for(const auto& c:r.channels) out << ',' << quote(c.name+"["+c.unit+"]{"+c.object+"}");
    for(const auto& id:r.gate_objects) out << ',' << quote("gate{"+id+"}[bool]");
    out << '\n';
    for(const auto& s:r.samples) {
        out << s.time;
        for(double v:s.values) out << ',' << v;
        for(bool g:s.gates) out << ',' << g;
        out << '\n';
    }
    if(!out) throw Diagnostic("write_error",r.project_id,"Result export failed");
}
}
