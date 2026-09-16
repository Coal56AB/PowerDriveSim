#include "results/experiment_csv.hpp"
#include <iomanip>
#include <locale>
#include <sstream>

namespace pds {
namespace {
std::string quoted(const std::string &value) {
    std::string result = "\"";
    for (char c : value) {
        if (c == '"')
            result += '"';
        result += c;
    }
    return result + '"';
}
} // namespace
void write_experiment_csv_header(std::ostream &out) {
    out << "case,scenario,parameters,status,error_code,error_object,error_message,simulated_s,elapsed_s,"
           "steps,"
           "channel,unit,begin_s,end_s,mean,rms,stddev,integral,final,minimum,maximum\n";
}
void write_experiment_csv_case(std::ostream &out, const ExperimentCase &c) {
    std::ostringstream parameters, rows;
    parameters.imbue(std::locale::classic());
    rows.imbue(std::locale::classic());
    parameters << std::setprecision(17);
    rows << std::setprecision(17);
    for (size_t i = 0; i < c.parameters.size(); ++i) {
        const auto &p = c.parameters[i];
        if (i)
            parameters << "; ";
        for (const auto &instance : p.target.instances)
            parameters << instance << '/';
        parameters << (p.target.object.empty() ? "profile" : p.target.object) << '/' << p.target.field << '='
                   << p.value;
    }
    auto prefix = [&] {
        rows << c.index + 1 << ',' << quoted(c.scenario) << ',' << quoted(parameters.str()) << ','
             << (c.cancelled            ? "stopped"
                 : c.error_code.empty() ? "ok"
                                        : "failed")
             << ',' << quoted(c.error_code) << ',' << quoted(c.error_object) << ',' << quoted(c.error_message)
             << ',' << c.simulated_time << ',' << c.elapsed_seconds << ',' << c.accepted_steps << ',';
    };
    if (c.measurements.empty()) {
        prefix();
        rows << ",,,,,,,,,,\n";
    }
    for (const auto &m : c.measurements) {
        prefix();
        rows << quoted(m.channel.name) << ',' << quoted(m.channel.unit) << ',';
        if (m.time.intervals)
            rows << m.time.begin << ',' << m.time.end << ',' << m.time.mean << ',' << m.time.rms << ','
                 << m.time.standard_deviation << ',' << m.time.integral << ',';
        else
            rows << ",,,,,,";
        if (m.has_samples)
            rows << m.final_value << ',' << m.minimum << ',' << m.maximum;
        else
            rows << ",,";
        rows << '\n';
    }
    out << rows.str();
    if (!out)
        throw Diagnostic("write_error", "", "Cannot write experiment summary");
}
} // namespace pds
