#include "core/model/model.hpp"
#include <array>
#include <cstdint>
#include <iomanip>
#include <random>
#include <regex>
#include <sstream>
#include <cmath>
namespace pds {
static std::string uuid_bytes(std::array<unsigned char,16> bytes,unsigned version) {
    bytes[6]=static_cast<unsigned char>((bytes[6]&15)|(version<<4));
    bytes[8]=static_cast<unsigned char>((bytes[8]&63)|128);
    std::ostringstream s;
    for(size_t i=0;i<bytes.size();++i) {
        if(i==4||i==6||i==8||i==10) s<<'-';
        s<<std::hex<<std::setw(2)<<std::setfill('0')<<static_cast<unsigned>(bytes[i]);
    }
    return s.str();
}
std::string new_uuid() {
    std::random_device random; std::array<unsigned char,16> bytes{};
    for(auto& b:bytes) b=static_cast<unsigned char>(random());
    return uuid_bytes(bytes,4);
}
std::string derived_uuid(const std::string& key) {
    std::array<unsigned char,16> bytes{};
    for(unsigned lane=0;lane<2;++lane) {
        uint64_t hash=14695981039346656037ull ^ (lane?0x9e3779b97f4a7c15ull:0);
        for(unsigned char c:key) { hash^=c; hash*=1099511628211ull; }
        for(unsigned i=0;i<8;++i) bytes[lane*8+i]=static_cast<unsigned char>(hash>>(i*8));
    }
    return uuid_bytes(bytes,8); // Application-defined name-derived UUID, not UUIDv5.
}
std::string component_unit(Kind k) {
    switch(k) {
    case Kind::resistor:return "Ohm"; case Kind::capacitor:return "F";
    case Kind::inductor:return "H"; case Kind::voltage:return "V";
    case Kind::current:return "A"; default:return "";
    }
}
double parse_si(const std::string& text,const std::string& unit) {
    static const std::regex expression(R"(^\s*([+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?)\s*(.*?)\s*$)");
    std::smatch match;
    if(!std::regex_match(text,match,expression)) throw Diagnostic("invalid_parameter","","Enter a number with an SI prefix and matching unit");
    double value=0;
    try { value=std::stod(match[1].str()); }
    catch(const std::exception&) { throw Diagnostic("invalid_parameter","","Number is outside the supported range"); }
    std::string suffix=match[2];
    if(suffix=="\xce\xa9") suffix="Ohm";
    for(const auto& micro:{std::string("\xc2\xb5"),std::string("\xce\xbc")})
        if(suffix.rfind(micro,0)==0) suffix="u"+suffix.substr(micro.size());
    double scale=1;
    if(suffix!=unit && !suffix.empty()) {
        const std::string prefixes="pnumkMG";
        const std::array<double,7> scales={1e-12,1e-9,1e-6,1e-3,1e3,1e6,1e9};
        const auto index=prefixes.find(suffix[0]);
        if(index==std::string::npos) throw Diagnostic("incompatible_unit","","Expected "+unit);
        scale=scales[index]; suffix.erase(0,1);
        if(suffix=="\xce\xa9") suffix="Ohm";
    }
    if(!suffix.empty() && suffix!=unit) throw Diagnostic("incompatible_unit","","Expected "+unit);
    value*=scale;
    if(!std::isfinite(value)) throw Diagnostic("invalid_parameter","","Parameter must be finite");
    return value;
}
}
