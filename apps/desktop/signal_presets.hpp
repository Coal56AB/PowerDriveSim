#pragma once

#include "core/model/model.hpp"

#include <array>

namespace pds::desktop {

// Library palette metadata remains in component JSON. These presets stay ordinary
// CodeBlocks and share one factory for the placement ghost and inserted model.
struct SignalPreset {
    int placement_id;
    const char *code;
    double period;
};

const SignalPreset *signal_preset(int placement_id);
CodeBlock make_signal_preset(const SignalPreset &preset, const std::string &name, double x, double y);

} // namespace pds::desktop
