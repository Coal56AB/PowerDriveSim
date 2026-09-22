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
    if ((s.version < 1 || s.version > 4) || !valid_uuid(s.project_id) || !valid_uuid(s.contract) || !std::isfinite(s.time) ||
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
    if(s.version>=3) {
        if(s.gate_program_states.size()>maximum_values)invalid();
        stream<<"gate_program_states "<<s.gate_program_states.size()<<'\n';
        for(const auto &state:s.gate_program_states) {
            if(state.static_values.size()>maximum_values||state.initialized.size()>maximum_values)invalid();
            stream<<"gate_program_state "<<state.static_values.size();
            for(const auto &[key,value]:state.static_values) {
                if(!std::isfinite(value))invalid();
                stream<<' '<<key<<' '<<value;
            }
            stream<<' '<<state.initialized.size();
            for(const auto &[key,value]:state.initialized)stream<<' '<<key<<' '<<value;
            stream<<'\n';
        }
    }
    if(s.version>=4) {
        if(s.signal_tasks.size()>maximum_values)invalid();
        stream<<"signal_tasks "<<s.signal_tasks.size()<<'\n';
        for(const auto &[id,task]:s.signal_tasks) {
            if(!valid_uuid(id)||task.outputs.size()>maximum_values||
               task.program_state.static_values.size()>maximum_values||
               task.program_state.initialized.size()>maximum_values)invalid();
            stream<<"signal_task "<<std::quoted(id)<<' '<<task.next_tick<<' '<<task.outputs.size();
            for(const auto &[name,value]:task.outputs) {
                if(name.empty()||!std::isfinite(value))invalid();
                stream<<' '<<std::quoted(name)<<' '<<value;
            }
            stream<<' '<<task.program_state.static_values.size();
            for(const auto &[key,value]:task.program_state.static_values) {
                if(!std::isfinite(value))invalid();
                stream<<' '<<key<<' '<<value;
            }
            stream<<' '<<task.program_state.initialized.size();
            for(const auto &[key,value]:task.program_state.initialized)
                stream<<' '<<key<<' '<<value;
            stream<<'\n';
        }
        const auto frame=[&](const char *key,const SignalFrame &values) {
            if(values.size()>maximum_values)invalid();
            stream<<key<<' '<<values.size()<<'\n';
            for(const auto &[endpoint,value]:values) {
                if(endpoint.empty()||!std::isfinite(value.value)||!std::isfinite(value.time)||value.time<0||
                   (value.type!=SignalScalarType::real&&value.type!=SignalScalarType::boolean)||
                   (value.type==SignalScalarType::boolean&&value.value!=0&&value.value!=1))invalid();
                stream<<"signal_value "<<std::quoted(endpoint)<<' '<<unsigned(value.type)<<' '
                      <<std::quoted(value.unit)<<' '<<value.value<<' '<<value.time<<' '
                      <<value.valid<<'\n';
            }
        };
        frame("signal_outputs",s.signal_outputs);
        frame("accepted_signal_inputs",s.accepted_signal_inputs);
    } else if(!s.signal_tasks.empty()||!s.signal_outputs.empty()||!s.accepted_signal_inputs.empty())invalid();
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
    if (!(stream >> s.version) || s.version < 1 || s.version > 4)
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
    if(s.version>=3) {
        const auto programs=count("gate_program_states");
        s.gate_program_states.resize(programs);
        for(auto &state:s.gate_program_states) {
            token("gate_program_state");
            size_t static_count=0;
            if(!(stream>>static_count)||static_count>maximum_values)invalid();
            for(size_t i=0;i<static_count;++i) {
                size_t key=0;double value=0;
                if(!(stream>>key>>value)||!std::isfinite(value)||!state.static_values.emplace(key,value).second)invalid();
            }
            size_t initialized_count=0;
            if(!(stream>>initialized_count)||initialized_count>maximum_values)invalid();
            for(size_t i=0;i<initialized_count;++i) {
                size_t key=0;unsigned value=0;
                if(!(stream>>key>>value)||value>1||!state.initialized.emplace(key,value!=0).second)invalid();
            }
        }
    }
    if(s.version>=4) {
        const auto tasks=count("signal_tasks");
        for(size_t i=0;i<tasks;++i) {
            token("signal_task");
            std::string id; SignalTaskSnapshot task;
            size_t outputs=0;
            if(!(stream>>std::quoted(id)>>task.next_tick>>outputs)||!valid_uuid(id)||outputs>maximum_values)
                invalid();
            for(size_t j=0;j<outputs;++j) {
                std::string name;double value=0;
                if(!(stream>>std::quoted(name)>>value)||name.empty()||!std::isfinite(value)||
                   !task.outputs.emplace(name,value).second)invalid();
            }
            size_t static_count=0;
            if(!(stream>>static_count)||static_count>maximum_values)invalid();
            for(size_t j=0;j<static_count;++j) {
                size_t key=0;double value=0;
                if(!(stream>>key>>value)||!std::isfinite(value)||
                   !task.program_state.static_values.emplace(key,value).second)invalid();
            }
            size_t initialized_count=0;
            if(!(stream>>initialized_count)||initialized_count>maximum_values)invalid();
            for(size_t j=0;j<initialized_count;++j) {
                size_t key=0;unsigned value=0;
                if(!(stream>>key>>value)||value>1||
                   !task.program_state.initialized.emplace(key,value!=0).second)invalid();
            }
            if(!s.signal_tasks.emplace(id,std::move(task)).second)invalid();
        }
        const auto frame=[&](const char *key,SignalFrame &values) {
            const auto size=count(key);
            for(size_t i=0;i<size;++i) {
                token("signal_value");
                std::string endpoint,unit;unsigned type=0,valid=0;
                double value=0,time=0;
                if(!(stream>>std::quoted(endpoint)>>type>>std::quoted(unit)>>value>>time>>valid)||
                   endpoint.empty()||type>unsigned(SignalScalarType::boolean)||valid>1||
                   !std::isfinite(value)||!std::isfinite(time)||time<0||
                   (type==unsigned(SignalScalarType::boolean)&&value!=0&&value!=1)||
                   !values.emplace(endpoint,SignalValue{SignalScalarType(type),unit,value,time,valid!=0}).second)
                    invalid();
            }
        };
        frame("signal_outputs",s.signal_outputs);
        frame("accepted_signal_inputs",s.accepted_signal_inputs);
    }
    token("end");
    stream >> std::ws;
    if (!stream.eof())
        invalid();
    return s;
}
} // namespace pds
