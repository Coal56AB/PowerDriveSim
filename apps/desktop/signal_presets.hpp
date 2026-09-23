#pragma once

#include "core/model/model.hpp"

#include <array>
#include <span>

namespace pds::desktop {

// Library palette metadata remains in component JSON. These presets stay ordinary
// CodeBlocks and share one factory for the placement ghost and inserted model.
struct SignalPortPreset {
    const char *name;
    SignalScalarType type;
};

struct SignalPreset {
    int placement_id;
    const char *code;
    double period;
    std::span<const SignalPortPreset> inputs;
    std::span<const SignalPortPreset> outputs;
};

const SignalPreset *signal_preset(int placement_id);
std::vector<IconPrimitive> default_code_icon(int placement_id);
CodeBlock make_signal_preset(const SignalPreset &preset, const std::string &name, double x, double y);

} // namespace pds::desktop
