#pragma once

#include "core/model/model.hpp"

#include <map>
#include <string>
#include <vector>

namespace pds {

struct SignalEndpointIR {
    std::string object, port;
    bool operator==(const SignalEndpointIR &) const = default;
};

struct SignalPortIR {
    std::string id, name, unit;
    SignalScalarType type = SignalScalarType::real;
    double initial = 0;
    bool operator==(const SignalPortIR &) const = default;
};

struct SignalInputIR {
    SignalPortIR port;
    SignalEndpointIR source;
    bool operator==(const SignalInputIR &) const = default;
};

struct SignalTaskIR {
    std::string id;
    double period = 1e-6, phase = 0;
    std::string code;
    std::vector<SignalInputIR> inputs;
    std::vector<SignalPortIR> outputs;
    bool operator==(const SignalTaskIR &) const = default;
};

struct SignalIR {
    std::vector<SignalTaskIR> tasks;
    bool operator==(const SignalIR &) const = default;
};

struct SignalValue {
    SignalScalarType type = SignalScalarType::real;
    std::string unit;
    double value = 0, time = 0;
    bool valid = false;
    bool operator==(const SignalValue &) const = default;
};

using SignalFrame = std::map<std::string, SignalValue>;

std::string signal_endpoint_key(const SignalEndpointIR &endpoint);
void validate_signal_ir(const SignalIR &ir);

} // namespace pds
