#pragma once
#include <map>
#include <string>

namespace pds {

struct ExpressionProgram {
    std::map<std::string, std::string> variables;
    std::string expression;
};

struct ExpressionOptions {
    std::string diagnostic_code = "invalid_expression";
    std::string object;
    bool allow_time = false;
    bool allow_gate_functions = false;
};

ExpressionProgram parse_expression_program(const std::string &source,
                                           const ExpressionOptions &options = {});
double evaluate_expression(const std::string &expression,
                           const std::map<std::string, std::string> &variables = {},
                           double time = 0,
                           const ExpressionOptions &options = {});
bool expression_depends_on_time(const std::string &expression,
                                const std::map<std::string, std::string> &variables = {});

} // namespace pds
