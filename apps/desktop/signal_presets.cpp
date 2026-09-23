#include "apps/desktop/signal_presets.hpp"

namespace pds::desktop {
namespace {
constexpr std::array<SignalPortPreset, 1> real_output{{{"out", SignalScalarType::real}}};
constexpr std::array<SignalPortPreset, 1> real_input{{{"in", SignalScalarType::real}}};
constexpr std::array<SignalPortPreset, 2> real_pair{{
    {"a", SignalScalarType::real}, {"b", SignalScalarType::real},
}};
constexpr std::array<SignalPortPreset, 2> real_reference_carrier{{
    {"reference", SignalScalarType::real}, {"carrier", SignalScalarType::real},
}};
constexpr std::array<SignalPortPreset, 2> real_sample_inputs{{
    {"in", SignalScalarType::real}, {"sample", SignalScalarType::boolean},
}};
constexpr std::array<SignalPortPreset, 1> boolean_output{{{"out", SignalScalarType::boolean}}};
constexpr std::array<SignalPortPreset, 2> boolean_pair{{
    {"a", SignalScalarType::boolean}, {"b", SignalScalarType::boolean},
}};

constexpr std::array<SignalPreset, 13> presets{{
    {109, "out = 1;", 100e-6, {}, real_output},
    {110, "out = t >= 5e-3 ? 1 : 0;", 100e-6, {}, real_output},
    {111, "out = t < 10e-3 ? t / 10e-3 : 1;", 100e-6, {}, real_output},
    {112, "out = sin(2 * PI * 50 * t);", 100e-6, {}, real_output},
    {113, "out = a + b;", 100e-6, real_pair, real_output},
    {114, "out = clamp(in, -1, 1);", 100e-6, real_input, real_output},
    {115, "out = a >= b;", 100e-6, real_pair, boolean_output},
    {116, "out = a && b;", 100e-6, boolean_pair, boolean_output},
    {117, "static double integral = 0;\nintegral += in * dt;\nout = integral;", 100e-6, real_input, real_output},
    {118, "static double previous = 0;\nout = previous;\nprevious = in;", 100e-6, real_input, real_output},
    {119, "double phase = t * 1000 - floor(t * 1000);\nout = 1 - 4 * abs(phase - 0.5);", 100e-6, {}, real_output},
    {120, "out = reference >= carrier;", 100e-6, real_reference_carrier, boolean_output},
    {121, "static double held = 0;\nif (sample) held = in;\nout = held;", 100e-6, real_sample_inputs, real_output},
}};
} // namespace

const SignalPreset *signal_preset(int placement_id) {
    for (const auto &preset : presets)
        if (preset.placement_id == placement_id)
            return &preset;
    return nullptr;
}

std::vector<IconPrimitive> default_code_icon(int placement_id) {
    auto line=[](std::initializer_list<Point> points, IconColor color=IconColor::foreground) {
        return IconPrimitive{points.size()>2?IconPrimitiveKind::polyline:IconPrimitiveKind::line,
                             color,false,{},std::vector<Point>(points)};
    };
    auto text=[](const char *value, double size=13) {
        return IconPrimitive{IconPrimitiveKind::text,IconColor::foreground,false,value,{{16,16},{size,0}}};
    };
    switch(placement_id) {
    case 109:return {text("1")};
    case 110:return {line({{4,24},{13,24},{13,8},{28,8}},IconColor::signal)};
    case 111:return {line({{4,25},{27,7}},IconColor::signal)};
    case 112:return {line({{3,16},{6,10},{9,7},{12,10},{16,22},{20,25},{23,22},{29,10}},IconColor::signal)};
    case 113:return {text("+")};
    case 114:return {line({{3,24},{9,24},{23,8},{29,8}},IconColor::signal)};
    case 115:return {text(">=")};
    case 116:return {text("AND",8)};
    case 117:return {text("INT",8)};
    case 118:return {text("z^-1",7)};
    case 119:return {line({{3,24},{10,8},{17,24},{24,8},{29,20}},IconColor::signal)};
    case 120:return {line({{3,23},{8,23},{8,9},{15,9},{15,23},{22,23},{22,9},{29,9}},IconColor::gate)};
    case 121:return {text("S/H",8)};
    default:return {text("{C}",8)};
    }
}

CodeBlock make_signal_preset(const SignalPreset &preset, const std::string &name, double x, double y) {
    CodeBlock block;
    block.id = new_uuid();
    block.name = name;
    block.x = x;
    block.y = y;
    block.period = preset.period;
    block.code = preset.code;
    block.icon = default_code_icon(preset.placement_id);
    for (const auto &port : preset.inputs)
        block.inputs.push_back({new_uuid(), port.name, "", port.type, 0});
    for (const auto &port : preset.outputs)
        block.outputs.push_back({new_uuid(), port.name, "", port.type, 0});
    return block;
}

} // namespace pds::desktop
