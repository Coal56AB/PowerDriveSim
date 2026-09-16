#include "formats/experiment/experiment.hpp"
#include "core/model/model.hpp"
#include <cmath>
#include <iomanip>
#include <istream>
#include <ostream>

namespace pds {
namespace {
void fail() {
    throw Diagnostic("parse_error", "", "Invalid experiment record");
}
void text(const std::string &s) {
    if (s.find_first_of("\r\n") != std::string::npos)
        throw Diagnostic("invalid_text", "", "Experiment strings must be single-line");
}
size_t count(std::istream &in, size_t maximum = 100000) {
    size_t n = 0;
    if (!(in >> n) || n > maximum)
        fail();
    return n;
}
ParameterTarget target(std::istream &in) {
    ParameterTarget t;
    t.instances.resize(count(in, 64));
    for (auto &id : t.instances)
        in >> std::quoted(id);
    in >> std::quoted(t.object) >> std::quoted(t.field);
    if (!in || t.field.empty())
        fail();
    return t;
}
void target(std::ostream &out, const ParameterTarget &t) {
    out << t.instances.size();
    for (const auto &id : t.instances) {
        text(id);
        out << ' ' << std::quoted(id);
    }
    text(t.object);
    text(t.field);
    out << ' ' << std::quoted(t.object) << ' ' << std::quoted(t.field);
}
double number(std::istream &in) {
    double value = 0;
    if (!(in >> value) || !std::isfinite(value))
        fail();
    return value;
}
} // namespace
Experiment read_experiment(std::istream &in) {
    Experiment e;
    int retain = -1;
    in >> std::quoted(e.id) >> std::quoted(e.name);
    e.begin = number(in);
    e.end = number(in);
    in >> retain;
    if (!valid_uuid(e.id) || (retain != 0 && retain != 1))
        fail();
    e.retain_curves = retain == 1;
    e.channels.resize(count(in));
    for (auto &channel : e.channels)
        in >> std::quoted(channel);
    e.axes.resize(count(in, 64));
    size_t budget = 100000;
    for (auto &axis : e.axes) {
        axis.target = target(in);
        const size_t n = count(in, budget);
        budget -= n;
        axis.values.resize(n);
        for (auto &v : axis.values)
            v = number(in);
    }
    e.scenarios.resize(count(in));
    for (auto &scenario : e.scenarios) {
        in >> std::quoted(scenario.name);
        const size_t n = count(in, budget);
        budget -= n;
        scenario.overrides.resize(n);
        for (auto &v : scenario.overrides) {
            v.target = target(in);
            v.value = number(in);
        }
    }
    if (!in)
        fail();
    (void)experiment_size(e);
    return e;
}
void write_experiment(std::ostream &out, const Experiment &e) {
    (void)experiment_size(e);
    text(e.id);
    text(e.name);
    out << std::quoted(e.id) << ' ' << std::quoted(e.name) << ' ' << e.begin << ' ' << e.end << ' '
        << e.retain_curves << ' ' << e.channels.size();
    for (const auto &channel : e.channels) {
        text(channel);
        out << ' ' << std::quoted(channel);
    }
    out << ' ' << e.axes.size();
    for (const auto &axis : e.axes) {
        out << ' ';
        target(out, axis.target);
        out << ' ' << axis.values.size();
        for (double v : axis.values)
            out << ' ' << v;
    }
    out << ' ' << e.scenarios.size();
    for (const auto &scenario : e.scenarios) {
        text(scenario.name);
        out << ' ' << std::quoted(scenario.name) << ' ' << scenario.overrides.size();
        for (const auto &v : scenario.overrides) {
            out << ' ';
            target(out, v.target);
            out << ' ' << v.value;
        }
    }
}
} // namespace pds
