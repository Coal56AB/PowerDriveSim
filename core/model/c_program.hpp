#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

namespace pds {

struct CProgramOptions {
    std::string diagnostic_code = "invalid_c_program";
    std::string object;
    bool allow_time = false;
    bool allow_gate_functions = false;
    bool require_return = false;
    std::size_t instruction_budget = 100000;
    std::size_t call_depth_limit = 32;
    // Declared by the embedding runtime rather than by user code. Inputs are
    // read-only unless explicitly listed as writable outputs.
    std::set<std::string> external_variables;
    std::set<std::string> writable_variables;
};

struct CProgramState {
    std::map<std::size_t, double> static_values;
    std::map<std::size_t, bool> initialized;
    bool operator==(const CProgramState &) const = default;
};

struct CProgramResult {
    std::map<std::string, double> variables;
    std::optional<double> return_value;
};

class CProgram {
public:
    CProgram() = default;
    bool valid() const noexcept { return static_cast<bool>(implementation_); }

private:
    struct Implementation;
    std::shared_ptr<const Implementation> implementation_;
    explicit CProgram(std::shared_ptr<const Implementation> implementation)
        : implementation_(std::move(implementation)) {}
    friend CProgram compile_c_program(const std::string &, const CProgramOptions &);
    friend CProgramResult execute_c_program(const CProgram &, double, CProgramState *,
                                             const std::map<std::string, double> &);
};

CProgram compile_c_program(const std::string &source, const CProgramOptions &options = {});
CProgramResult execute_c_program(const CProgram &program, double time = 0,
                                 CProgramState *state = nullptr,
                                 const std::map<std::string, double> &inputs = {});

} // namespace pds
