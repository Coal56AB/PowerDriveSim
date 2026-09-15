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
        << "; precision=" << r.precision << "; method=BackwardEuler; step=" << std::setprecision(17) << r.profile.step
        << "; stop=" << r.profile.stop << "; cancelled=" << r.cancelled << "\ntime[s]";
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