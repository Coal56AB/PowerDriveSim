#include "apps/desktop/signal_presets.hpp"

namespace pds::desktop {
namespace {
constexpr std::array<SignalPreset, 4> presets{{
    {109, "out = 1;", 100e-6},
    {110, "out = t >= 5e-3 ? 1 : 0;", 100e-6},
    {111, "out = t < 10e-3 ? t / 10e-3 : 1;", 100e-6},
    {112, "out = sin(2 * PI * 50 * t);", 100e-6},
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
    block.outputs.push_back({new_uuid(), "out", "", SignalScalarType::real, 0});
    return block;
}

} // namespace pds::desktop
