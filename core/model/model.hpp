#pragma once
#include <stdexcept>
#include <string>
#include <vector>
namespace pds {
enum class Kind { resistor, capacitor, inductor, voltage, current, ideal_switch, diode, voltage_probe, current_probe };
std::string kind_name(Kind kind);
Kind parse_kind(const std::string& name);
struct Node { std::string id, name; bool ground = false; double x=0, y=0; };
struct Component {
    std::string id, name;
    Kind kind = Kind::resistor;
    std::string positive, negative;
    double value = 1.0, initial = 0.0, x = 0.0, y = 0.0;
    bool closed = false;
};
struct GateEvent { double time; std::string target; bool closed; };
enum class Method { backward_euler, trapezoidal };
std::string method_name(Method method);
Method parse_method(const std::string& name);
struct Profile {
    double stop = 0.01, step = 0.00001;
    Method method = Method::backward_euler;
    unsigned max_iterations = 64;
    double voltage_tolerance = 1e-9, current_tolerance = 1e-12, relative_tolerance = 1e-9;
};
struct Point { double x=0,y=0; };
struct Endpoint {
    std::string object, port;
    bool operator==(const Endpoint&) const = default;
};
struct Wire { std::string id; Endpoint from,to; std::vector<Point> bends; };
struct GatePattern { std::string id,name; double x=0,y=0; bool initial=false; };
enum class Domain { electrical, gate, signal };
enum class Direction { conserving, input, output };
struct PortType { Domain domain; Direction direction; };
struct Project {
    unsigned schema = 4;
    std::string id, name;
    std::vector<Node> nodes;
    std::vector<Component> components;
    std::vector<GateEvent> events;
    Profile profile;
    std::vector<std::string> extensions;
    bool wired=false;
    std::vector<Wire> wires;
    std::vector<GatePattern> patterns;
    std::vector<std::string> scope_channels;
    double scope_begin=0,scope_end=-1,cursor_a=-1,cursor_b=-1;

};
struct Diagnostic : std::runtime_error {
    std::string code, object;
    double time;
    Diagnostic(std::string code, std::string object, std::string message, double time = 0.0);
};
bool valid_uuid(const std::string& value);
std::string new_uuid();
std::string derived_uuid(const std::string& key);
std::string component_unit(Kind kind);
double parse_si(const std::string& text,const std::string& unit);

}