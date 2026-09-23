#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
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
    // Fixed-size arrays supplied by the embedding runtime. Array storage is
    // bounded at compile time; user code may address it with a checked index.
    std::map<std::string, std::size_t> external_arrays;
    std::set<std::string> writable_arrays;
};

class CProgram;

struct CProgramState {
    std::map<std::size_t, double> static_values;
    std::map<std::size_t, bool> initialized;
    CProgramState() = default;
    CProgramState(const CProgramState &other):static_values(other.static_values),initialized(other.initialized) {}
    CProgramState &operator=(const CProgramState &other) {static_values=other.static_values;initialized=other.initialized;transient.reset();return *this;}
    CProgramState(CProgramState &&) noexcept = default;
    CProgramState &operator=(CProgramState &&) noexcept = default;
    bool operator==(const CProgramState &other) const {return static_values==other.static_values&&initialized==other.initialized;}
private:
    std::shared_ptr<void> transient;
    friend std::optional<double> execute_c_program_array(const CProgram &, double, CProgramState *, std::span<double>);
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
    friend std::optional<double> execute_c_program_array(const CProgram &, double, CProgramState *,
                                                         std::span<double>);
};

CProgram compile_c_program(const std::string &source, const CProgramOptions &options = {});
CProgramResult execute_c_program(const CProgram &program, double time = 0,
                                 CProgramState *state = nullptr,
                                 const std::map<std::string, double> &inputs = {});
std::optional<double> execute_c_program_array(const CProgram &program, double time,
                                              CProgramState *state, std::span<double> values);

} // namespace pds
