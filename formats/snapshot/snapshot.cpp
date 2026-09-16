#include "formats/snapshot/snapshot.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <istream>
#include <limits>
#include <locale>
#include <ostream>
namespace pds {
namespace {
constexpr size_t maximum_values = 2'000'000;
struct StreamFormat {
    std::ios &stream;
    std::locale locale;
    std::ios::fmtflags flags;
    std::streamsize precision;
    explicit StreamFormat(std::ios &s)
        : stream(s), locale(s.getloc()), flags(s.flags()), precision(s.precision()) {
        stream.imbue(std::locale::classic());
        stream.setf(std::ios::dec, std::ios::basefield);
        stream.unsetf(std::ios::floatfield | std::ios::boolalpha);
        stream.precision(std::numeric_limits<double>::max_digits10);
    }
    ~StreamFormat() {
        stream.imbue(locale);
        stream.flags(flags);
        stream.precision(precision);
    }
};
[[noreturn]] void invalid() {
    throw Diagnostic("snapshot_format", "", "Invalid or unsupported state file");
}
void check_header(const SimulationSnapshot &s) {
    if ((s.version != 1 && s.version != 2) || !valid_uuid(s.project_id) || !valid_uuid(s.contract) || !std::isfinite(s.time) ||
        s.time < 0 || s.next_grid == 0 || !std::isfinite(s.next_step) || s.next_step < 0 || (s.version == 1 && s.next_step != 0))
        invalid();
}
} // namespace
void write_snapshot(const SimulationSnapshot &s, std::ostream &stream) {
    check_header(s);
    StreamFormat format(stream);
    stream << "PowerDriveSimSnapshot " << s.version << "\nproject " << std::quoted(s.project_id)
           << "\ncontract " << std::quoted(s.contract) << "\ntime " << s.time << "\ngrid " << s.next_grid
           << '\n';
    if (s.version >= 2) stream << "next_step " << s.next_step << '\n';
    const auto values = [&](const char *key, const auto &items) {
        if (items.size() > maximum_values)
            invalid();
        stream << key << ' ' << items.size();
        for (auto item : items) {
            if (!std::isfinite(static_cast<double>(item)))
                invalid();
            stream << ' ' << item;
        }
        stream << '\n';
    };
    values("states", s.states);
    values("history", s.history);
    values("values", s.values);
    values("gates", s.gates);
    values("diodes", s.diodes);
    values("latched", s.latched);
    values("signals", s.signal_values);
    stream << "end\n";
    if (!stream)
        throw Diagnostic("snapshot_write", "", "Could not write state file");
}
SimulationSnapshot read_snapshot(std::istream &stream) {
    StreamFormat format(stream);
    const auto token = [&](const char *expected) {
        std::string value;
        if (!(stream >> value) || value != expected)
            invalid();
    };
    SimulationSnapshot s;
    token("PowerDriveSimSnapshot");
    if (!(stream >> s.version) || (s.version != 1 && s.version != 2))
        invalid();
    token("project");
    stream >> std::quoted(s.project_id);
    token("contract");
    stream >> std::quoted(s.contract);
    token("time");
    stream >> s.time;
    token("grid");
    stream >> s.next_grid;
    if (s.version >= 2) {
        token("next_step");
        stream >> s.next_step;
    }
    if (!stream)
        invalid();
    check_header(s);
    const auto count = [&](const char *key) {
        token(key);
        size_t size = 0;
        if (!(stream >> size) || size > maximum_values)
            invalid();
        return size;
    };
    const auto values = [&](const char *key, std::vector<double> &items) {
        items.resize(count(key));
        for (auto &value : items)
            if (!(stream >> value) || !std::isfinite(value))
                invalid();
    };
    const auto bits = [&](const char *key, std::vector<bool> &items) {
        items.resize(count(key));
        for (size_t k = 0; k < items.size(); ++k) {
            unsigned value;
            if (!(stream >> value) || value > 1)
                invalid();
            items[k] = value != 0;
        }
    };
    values("states", s.states);
    values("history", s.history);
    values("values", s.values);
    bits("gates", s.gates);
    bits("diodes", s.diodes);
    bits("latched", s.latched);
    bits("signals", s.signal_values);
    token("end");
    stream >> std::ws;
    if (!stream.eof())
        invalid();
    return s;
}
} // namespace pds
