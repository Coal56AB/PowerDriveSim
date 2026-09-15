#include "core/model/model.hpp"
#include <cctype>
namespace pds {
std::string kind_name(Kind k) {
    switch(k) {
    case Kind::resistor: return "R";
    case Kind::capacitor: return "C";
    case Kind::inductor: return "L";
    case Kind::voltage: return "V";
    case Kind::current: return "I";
    case Kind::ideal_switch: return "S";
    }
    throw Diagnostic("unknown_component", "", "Unsupported component kind");
}
Kind parse_kind(const std::string& s) {
    for(auto k : {Kind::resistor, Kind::capacitor, Kind::inductor, Kind::voltage, Kind::current, Kind::ideal_switch})
        if(kind_name(k) == s) return k;
    throw Diagnostic("unknown_component", s, "Unsupported component type");
}
Diagnostic::Diagnostic(std::string c, std::string o, std::string m, double t)
    : std::runtime_error(std::move(m)), code(std::move(c)), object(std::move(o)), time(t) {}
bool valid_uuid(const std::string& s) {
    if(s.size()!=36) return false;
    for(size_t i=0;i<s.size();++i) {
        if(i==8 || i==13 || i==18 || i==23) { if(s[i]!='-') return false; }
        else if(!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
        else if(s[i]>='A' && s[i]<='F') return false;
    }
    return true;
}
}