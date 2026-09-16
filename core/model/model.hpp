#pragma once
#include "core/model/experiment.hpp"
#include <utility>
#include <stdexcept>
#include <string>
#include <vector>
namespace pds {
inline constexpr unsigned project_schema = 17;
enum class Kind { resistor, capacitor, inductor, voltage, current, ideal_switch, diode, voltage_probe, current_probe, thyristor, igbt };
inline bool gate_controlled(Kind kind) { return kind == Kind::ideal_switch || kind == Kind::thyristor || kind == Kind::igbt; }
inline bool rectifying(Kind kind) { return kind == Kind::diode || kind == Kind::thyristor || kind == Kind::igbt; }
std::string kind_name(Kind kind);
Kind parse_kind(const std::string& name);
struct Orientation {
    unsigned quarter_turns = 0;
    bool mirrored = false;
    double scale = 1.0;
    double scale_x = 1.0, scale_y = 1.0;
    bool operator==(const Orientation &) const = default;
};
struct Node { std::string id, name; bool ground = false; double x=0, y=0; Orientation orientation; bool operator==(const Node&) const = default; };
struct Point { double x=0,y=0; bool operator==(const Point&) const = default; };
enum class Waveform { dc, sine, pulse, piecewise_linear };
struct SourceWaveform {
    Waveform kind=Waveform::dc;
    double offset=0, frequency=50, phase=0, delay=0, duty=.5;
    std::vector<Point> points;
    bool operator==(const SourceWaveform&) const = default;
};
enum class SemiconductorModel { ideal, piecewise_linear };
struct Semiconductor {
    SemiconductorModel model = SemiconductorModel::ideal;
    double ron = .01, roff = 1e6, forward_voltage = .7;
    bool charge_dynamics = false;
    double transit_time = 1e-6, carrier_lifetime = 5e-6, initial_charge = 0;
    double holding_current = 0;
    bool initial_latched = false;
    bool operator==(const Semiconductor &) const = default;
};
struct Component {
    std::string id, name;
    Kind kind = Kind::resistor;
    std::string positive, negative;
    double value = 1.0, initial = 0.0, x = 0.0, y = 0.0;
    bool closed = false;
    bool parallel_resistance_enabled = false;
    double parallel_resistance = 1e12;
    Orientation orientation;
    SourceWaveform source;
    Semiconductor semiconductor;
    bool operator==(const Component&) const = default;
};
struct GateEvent { double time; std::string target; bool closed; bool operator==(const GateEvent&) const = default; };
enum class Method { backward_euler, trapezoidal };
std::string method_name(Method method);
Method parse_method(const std::string& name);
enum class InitialState { specified, zero, dc_operating_point };
std::string initial_state_name(InitialState state);
InitialState parse_initial_state(const std::string &name);
struct StepControl {
    bool adaptive = false;
    double minimum_step = 1e-12;
    double relative_tolerance = 1e-4;
    double voltage_tolerance = 1e-6, current_tolerance = 1e-8, charge_tolerance = 1e-12;
    bool operator==(const StepControl &) const = default;
};
struct Profile {
    double stop = 0.01, step = 0.00001;
    Method method = Method::backward_euler;
    unsigned max_iterations = 64;
    double voltage_tolerance = 1e-9, current_tolerance = 1e-12, relative_tolerance = 1e-9;
    InitialState initial_state = InitialState::specified;
    double warmup = 0; // Unrecorded interval [0, warmup); time remains absolute.
    StepControl step_control;
    bool operator==(const Profile&) const = default;
};
void validate_step_control(const Profile &profile, const std::string &object);
struct Endpoint {
    std::string object, port;
    bool operator==(const Endpoint&) const = default;
};
enum class Domain { electrical, gate, signal };
enum class Direction { conserving, input, output };
struct Wire { std::string id; Endpoint from,to; std::vector<Point> bends; bool operator==(const Wire&) const = default; };
struct ConnectionTag {
    std::string id,name;
    double x=0,y=0;
    Domain domain=Domain::electrical;
    Orientation orientation;
    bool operator==(const ConnectionTag&) const = default;
};
struct GatePattern {
    std::string id,name; double x=0,y=0; bool initial=false; Orientation orientation;
    bool pwm=false; double frequency=1000,duty=.5,delay=0;
    bool script=false; std::string code; double script_step=1e-6;
    bool operator==(const GatePattern&) const = default;
};
struct PortType { Domain domain; Direction direction; };
struct PlotBlock {
    std::string id,name; double x=0,y=0; unsigned inputs=2;
    double begin=0,end=-1,cursor_a=-1,cursor_b=-1;
    Orientation orientation;
    bool operator==(const PlotBlock&) const = default;
};
struct LabelLayout {
    std::string object,role;
    double x=0,y=0;
    Orientation orientation;
    bool operator==(const LabelLayout&) const = default;
};
enum class CurveLine { solid, dash, dot, dash_dot, none };
enum class CurveMarker { none, circle, square, triangle, diamond, cross, plus, triangle_down };
struct CurveStyle {
    std::string channel;
    CurveLine line=CurveLine::solid;
    double width=1.8;
    CurveMarker marker=CurveMarker::none;
    double marker_size=6;
    bool operator==(const CurveStyle&) const = default;
};
struct LegendPosition {
    unsigned display=0;
    double x=0,y=0;
    bool operator==(const LegendPosition&) const = default;
};
struct ViewOptions {
    unsigned display_columns=1;
    std::vector<std::pair<std::string,unsigned>> signal_displays;
    std::vector<std::string> hidden_channels;
    std::vector<CurveStyle> curve_styles;
    std::vector<std::pair<std::string, std::string>> curve_names;
    std::vector<std::pair<std::string, double>> curve_multipliers;
    std::vector<LegendPosition> legend_positions;
    std::string plot, cursor_channel_a, cursor_channel_b;
    double y_low=-1,y_high=1,cursor_y_a=0,cursor_y_b=0,time_span=0,line_width=1.8;
    bool manual_y=false,free_cursors=false,separate_axes=false,grid=true,legend=false;
    // Per-instance viewport override; absent values inherit the plot's defaults.
    bool viewport=false;
    double begin=0,end=-1,cursor_a=-1,cursor_b=-1;
    bool operator==(const ViewOptions&) const = default;
};
struct Instance {
    std::string id, name, definition;
    double x=0,y=0;
    Orientation orientation;
    std::vector<std::pair<std::string,double>> parameters;
    bool locked=false;
    bool operator==(const Instance&) const = default;
};
struct PublicPort {
    std::string id,name;
    Endpoint terminal;
    Domain domain=Domain::electrical;
    Direction direction=Direction::conserving;
    bool has_position=false;
    double x=0,y=0;
    bool operator==(const PublicPort&) const = default;
};
struct PublicParameter {
    std::string id,name,unit,object,field;
    double value=0;
    bool operator==(const PublicParameter&) const = default;
};
struct Schematic {
    std::vector<Node> nodes;
    std::vector<Component> components;
    std::vector<GateEvent> events;
    std::vector<std::string> extensions;
    bool wired=false;
    std::vector<Wire> wires;
    std::vector<ConnectionTag> tags;
    std::vector<GatePattern> patterns;
    std::vector<PlotBlock> plots;
    std::vector<LabelLayout> labels;
    std::vector<ViewOptions> view_options;
    std::vector<Instance> instances;
    bool operator==(const Schematic&) const = default;
};
struct Definition : Schematic {
    std::string id,name;
    std::vector<PublicPort> ports;
    std::vector<PublicParameter> parameters;
    bool operator==(const Definition&) const = default;
};
struct ObjectPath {
    std::vector<std::string> instances;
    std::string object;
    bool operator==(const ObjectPath&) const = default;
};
struct Project : Schematic {
    unsigned schema = project_schema;
    std::string id, name;
    Profile profile;
    std::vector<Experiment> experiments;
    std::vector<Definition> definitions;
    bool scope_enabled=false;
    std::vector<std::string> scope_channels;
    double scope_begin=0,scope_end=-1,cursor_a=-1,cursor_b=-1;
    bool operator==(const Project&) const = default;

};
struct Diagnostic : std::runtime_error {
    std::string code, object;
    std::vector<std::string> path;
    double time;
    Diagnostic(std::string code, std::string object, std::string message, double time = 0.0);
};
bool valid_uuid(const std::string& value);
std::string new_uuid();
std::string derived_uuid(const std::string& key);
std::string component_unit(Kind kind);
double parse_si(const std::string& text,const std::string& unit);

}
