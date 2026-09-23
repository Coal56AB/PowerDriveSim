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

CodeBlock make_signal_preset(const SignalPreset &preset, const std::string &name, double x, double y) {
    CodeBlock block;
    block.id = new_uuid();
    block.name = name;
    block.x = x;
    block.y = y;
    block.period = preset.period;
    block.code = preset.code;
    for (const auto &port : preset.inputs)
        block.inputs.push_back({new_uuid(), port.name, "", port.type, 0});
    for (const auto &port : preset.outputs)
        block.outputs.push_back({new_uuid(), port.name, "", port.type, 0});
    return block;
}

} // namespace pds::desktop
