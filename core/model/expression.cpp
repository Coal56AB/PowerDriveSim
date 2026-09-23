#include "core/model/expression.hpp"
#include "core/model/c_program.hpp"
#include "core/model/model.hpp"
#include "core/model/hierarchy.hpp"
#include "core/editor/properties.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <sstream>
#include <set>
#include <vector>

namespace pds {
namespace {

[[noreturn]] void fail(const ExpressionOptions &options, const std::string &message) {
    throw Diagnostic(options.diagnostic_code, options.object, message);
}

std::string compact_source(std::string text, const ExpressionOptions &options) {
    std::string result;
    for (size_t i = 0; i < text.size();) {
        if (i + 1 < text.size() && text[i] == '/' && text[i + 1] == '/') {
            i += 2;
            while (i < text.size() && text[i] != '\n') ++i;
        } else if (i + 1 < text.size() && text[i] == '/' && text[i + 1] == '*') {
            i += 2;
            while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) ++i;
            if (i + 1 == text.size()) fail(options, "Unterminated block comment");
            i += 2;
        } else {
            const auto c = static_cast<unsigned char>(text[i++]);
            if (!std::isspace(c)) result.push_back(char(std::tolower(c)));
        }
    }
    return result;
}

std::vector<std::string> split_statements(const std::string &text, const ExpressionOptions &options) {
    std::vector<std::string> result;
    size_t start = 0;
    int depth = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || (text[i] == ';' && depth == 0)) {
            auto statement = text.substr(start, i - start);
            if (!statement.empty()) result.push_back(std::move(statement));
            start = i + 1;
        } else if (text[i] == '(') ++depth;
        else if (text[i] == ')' && --depth < 0) fail(options, "Unbalanced parentheses");
    }
    if (depth != 0) fail(options, "Unbalanced parentheses");
    return result.empty() ? std::vector<std::string>{text} : result;
}

bool identifier(const std::string &value) {
    if (value.empty() || !(std::isalpha(static_cast<unsigned char>(value.front())) || value.front() == '_'))
        return false;
    return std::all_of(value.begin() + 1, value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_';
    });
}

class Parser {
public:
    Parser(const std::string &text, const std::map<std::string, std::string> &variables,
           double time, const ExpressionOptions &options, std::set<std::string> &resolving)
        : text_(text), variables_(variables), time_(time), options_(options), resolving_(resolving) {}

    double parse() {
        const double value = logical_or();
        if (position_ != text_.size()) error("Unexpected token");
        if (!std::isfinite(value)) error("Expression result must be finite");
        return value;
    }

private:
    [[noreturn]] void error(const std::string &message) const {
        fail(options_, message + " at column " + std::to_string(position_ + 1));
    }
    bool take(const std::string &token) {
        if (text_.compare(position_, token.size(), token) != 0) return false;
        position_ += token.size();
        return true;
    }
    double logical_or() { double v=logical_and(); while(take("||")){const auto r=logical_and();v=v!=0||r!=0;} return v; }
    double logical_and() { double v=equality(); while(take("&&")){const auto r=equality();v=v!=0&&r!=0;} return v; }
    double equality() {
        double v=comparison();
        for(;;) { if(take("=="))v=v==comparison(); else if(take("!="))v=v!=comparison(); else return v; }
    }
    double comparison() {
        double v=addition();
        for(;;) { if(take("<="))v=v<=addition(); else if(take(">="))v=v>=addition();
            else if(take("<"))v=v<addition(); else if(take(">"))v=v>addition(); else return v; }
    }
    double addition() {
        double v=multiplication();
        for(;;) { if(take("+"))v+=multiplication(); else if(take("-"))v-=multiplication(); else return v; }
    }
    double multiplication() {
        double v=unary();
        for(;;) { if(take("*"))v*=unary(); else if(take("/")){const auto d=unary();if(d==0)error("Division by zero");v/=d;} else return v; }
    }
    double unary() {
        if(take("!"))return unary()==0;
        if(take("+"))return unary();
        if(take("-"))return -unary();
        return primary();
    }
    double primary() {
        if(take("(")){const auto v=logical_or();if(!take(")"))error("Expected ')'");return v;}
        if(position_<text_.size()&&(std::isdigit(static_cast<unsigned char>(text_[position_]))||text_[position_]=='.')) {
            size_t used=0;double value=0;
            try { value=std::stod(text_.substr(position_),&used); }
            catch(const std::exception&) { error("Invalid number"); }
            position_+=used;return value;
        }
        const size_t begin=position_;
        if(position_<text_.size()&&(std::isalpha(static_cast<unsigned char>(text_[position_]))||text_[position_]=='_')) {
            ++position_;while(position_<text_.size()&&(std::isalnum(static_cast<unsigned char>(text_[position_]))||text_[position_]=='_'))++position_;
        }
        if(begin==position_)error("Expected a number, variable or function");
        const auto name=text_.substr(begin,position_-begin);
        if(take("(")) {
            std::vector<double> arguments;
            if(!take(")")){do arguments.push_back(logical_or());while(take(","));if(!take(")"))error("Expected ')' after function arguments");}
            return call(name,arguments);
        }
        if(name=="t") { if(!options_.allow_time)error("Variable 't' is not available here"); return time_; }
        if(name=="true")return 1;
        if(name=="false")return 0;
        const auto variable=variables_.find(name);
        if(variable==variables_.end())error("Unknown variable '"+name+"'");
        if(!resolving_.insert(name).second)error("Cyclic variable reference '"+name+"'");
        Parser nested(variable->second,variables_,time_,options_,resolving_);
        const auto value=nested.parse();resolving_.erase(name);return value;
    }
    double call(const std::string &name,const std::vector<double> &a) const {
        if(name=="ramp") {
            if(a.size()!=4)error("Use ramp(t0,t1,value0,value1)");
            if(a[1]<=a[0])error("Ramp end time must be greater than start time");
            const auto k=std::clamp((time_-a[0])/(a[1]-a[0]),0.0,1.0);return a[2]+(a[3]-a[2])*k;
        }
        if(name=="pwm"||name=="square"||name=="phasepwm") {
            if(a.size()!=3||a[0]<=0||a[1]<0||a[1]>1||(name!="phasepwm"&&a[2]<0))error("Use pwm(frequency,duty,delay) with duty 0..1");
            if(a[1]==0)return 0;
            if(a[1]==1)return 1;
            const auto period=1.0/a[0];double delay=name=="phasepwm"?std::fmod(a[2],period):a[2];
            if(delay<0)delay+=period;
            if(name!="phasepwm"&&time_<delay)return 0;
            double phase=std::fmod(time_-delay,period);if(phase<0)phase+=period;
            const double boundary=a[1]*period;
            const double tolerance=64*std::numeric_limits<double>::epsilon()*
                                   std::max({period,std::abs(time_),std::abs(delay)});
            return phase<boundary-tolerance;
        }
        if(name=="abs"&&a.size()==1)return std::abs(a[0]);
        if(name=="min"&&a.size()==2)return std::min(a[0],a[1]);
        if(name=="max"&&a.size()==2)return std::max(a[0],a[1]);
        if(name=="sqrt"&&a.size()==1&&a[0]>=0)return std::sqrt(a[0]);
        if(name=="pow"&&a.size()==2)return std::pow(a[0],a[1]);
        if(name=="clamp"&&a.size()==3&&a[1]<=a[2])return std::clamp(a[0],a[1],a[2]);
        if(name=="lerp"&&a.size()==3)return a[0]+(a[1]-a[0])*a[2];
        if(name=="saturate"&&a.size()==1)return std::clamp(a[0],0.0,1.0);
        if(name=="sign"&&a.size()==1)return (a[0]>0)-(a[0]<0);
        if(name=="step"&&a.size()==2)return a[1]>=a[0];
        if(name=="smoothstep"&&a.size()==3&&a[1]>a[0]){const auto x=std::clamp((a[2]-a[0])/(a[1]-a[0]),0.0,1.0);return x*x*(3-2*x);}
        if(name=="deadband"&&a.size()==2&&a[1]>=0)return std::abs(a[0])<=a[1]?0:a[0]-std::copysign(a[1],a[0]);
        if(name=="wrap"&&a.size()==2&&a[1]>0)return a[0]-std::floor(a[0]/a[1])*a[1];
        if(name=="pulse"&&a.size()==2&&a[1]>=0)return time_>=a[0]&&time_<a[0]+a[1];
        if((name=="saw"||name=="triangle")&&a.size()==2&&a[0]>0){const auto period=1.0/a[0];double phase=std::fmod(time_-a[1],period);if(phase<0)phase+=period;const auto unit=phase/period;return name=="saw"?unit:1-4*std::abs(unit-.5);}
        error("Unknown function or invalid argument count for '"+name+"'");
    }
    const std::string &text_;const std::map<std::string,std::string> &variables_;double time_;
    const ExpressionOptions &options_;std::set<std::string> &resolving_;size_t position_=0;
};

bool depends(const std::string &text,const std::map<std::string,std::string> &variables,std::set<std::string> &checking) {
    for(size_t i=0;i<text.size();) {
        if(!(std::isalpha(static_cast<unsigned char>(text[i]))||text[i]=='_')){++i;continue;}
        const size_t begin=i++;while(i<text.size()&&(std::isalnum(static_cast<unsigned char>(text[i]))||text[i]=='_'))++i;
        const auto name=text.substr(begin,i-begin);
        if(name=="t"||name=="ramp"||name=="pulse"||name=="pwm"||name=="square"||
           name=="phasepwm"||name=="saw"||name=="triangle")return true;
        if(const auto variable=variables.find(name);variable!=variables.end()&&checking.insert(name).second){
            const bool result=depends(variable->second,variables,checking);checking.erase(name);if(result)return true;
        }
    }
    return false;
}
} // namespace

ExpressionProgram parse_expression_program(const std::string &source,const ExpressionOptions &options) {
    const auto code=compact_source(source,options);ExpressionProgram program;
    for(auto statement:split_statements(code,options)) {
        if(statement.rfind("return",0)==0){program.expression=statement.substr(6);continue;}
        const auto eq=statement.find('=');
        const bool assignment=eq!=std::string::npos&&
            (eq==0||(statement[eq-1]!='<'&&statement[eq-1]!='>'&&statement[eq-1]!='!'&&statement[eq-1]!='='))&&
            (eq+1==statement.size()||statement[eq+1]!='=');
        if(assignment) {
            auto name=statement.substr(0,eq);
            for(const auto *prefix:{"constdouble","constbool","constauto","double","bool","auto"})
                if(name.rfind(prefix,0)==0){name=name.substr(std::char_traits<char>::length(prefix));break;}
            if(!identifier(name))fail(options,"Assignment target must be an identifier");
            if(program.variables.contains(name))fail(options,"Duplicate variable '"+name+"'");
            program.variables[name]=statement.substr(eq+1);
        } else program.expression=std::move(statement);
    }
    if(program.expression.empty()&&program.variables.empty())program.expression=code;
    return program;
}

double evaluate_expression(const std::string &expression,const std::map<std::string,std::string> &variables,
                           double time,const ExpressionOptions &options) {
    const auto compact=compact_source(expression,options);
    std::set<std::string> resolving;return Parser(compact,variables,time,options,resolving).parse();
}

bool expression_depends_on_time(const std::string &expression,const std::map<std::string,std::string> &variables) {
    const auto compact=compact_source(expression,{});
    std::set<std::string> checking;return depends(compact,variables,checking);
}

namespace {
std::map<std::string,std::string> initialization_variables(const Schematic &body,
                                                           const std::string &identity) {
    std::map<std::string,std::string> variables;
    if(!body.initialization_code.empty()) {
        CProgramOptions options;options.diagnostic_code="invalid_initialization";options.object=identity;
        const auto result=execute_c_program(compile_c_program(body.initialization_code,options));
        for(const auto &[name,value]:result.variables) {
            std::ostringstream text;text.precision(std::numeric_limits<double>::max_digits10);text<<value;
            variables.emplace(name,text.str());
        }
    }
    return variables;
}
double evaluate_public_parameter_default(const PublicParameter &parameter,
                                         const std::map<std::string,std::string> &variables) {
    const ExpressionOptions options{"invalid_parameter_expression",parameter.id,false,false};
    const double value=evaluate_expression(parameter.default_expression,variables,0,options);
    if(!std::isfinite(value))
        throw Diagnostic("invalid_parameter_expression",parameter.id,
                         "Public parameter default expression must be finite");
    return value;
}
void resolve_schematic(Project &body,const std::map<std::string,std::string> &variables) {
    std::set<std::pair<std::string,std::string>> bindings;
    for(const auto &binding:body.parameter_expressions) {
        if(binding.object.empty()||binding.field.empty()||binding.source.empty()||
           !bindings.emplace(binding.object,binding.field).second)
            throw Diagnostic("invalid_parameter_expression",binding.object,
                             "Parameter expression binding is empty or duplicated");
        const ExpressionOptions options{"invalid_parameter_expression",binding.object,false,false};
        const double value=evaluate_expression(binding.source,variables,0,options);
        try {
            const auto current=read_property(body,binding.object,binding.field);
            if(!std::holds_alternative<double>(current))
                throw Diagnostic("invalid_parameter_expression",binding.object,
                                 "Expressions can only target numeric properties");
            write_property(body,binding.object,binding.field,value);
        } catch(const Diagnostic &diagnostic) {
            if(diagnostic.code=="invalid_parameter_expression")throw;
            throw Diagnostic("invalid_parameter_expression",binding.object,diagnostic.what());
        }
    }
}
} // namespace

double public_parameter_default_value(const Definition &definition,
                                      const PublicParameter &parameter) {
    if(parameter.default_expression.empty())return parameter.value;
    if(parameter.default_expression.size()>1024*1024)
        throw Diagnostic("invalid_parameter_expression",parameter.id,
                         "Public parameter default expression exceeds 1 MiB");
    const auto variables=initialization_variables(definition,definition.id);
    return evaluate_public_parameter_default(parameter,variables);
}

Project resolve_parameter_expressions(const Project &source) {
    Project result=source;
    std::map<std::string,std::map<std::string,std::string>> definition_variables;
    for(auto &definition:result.definitions) {
        auto variables=initialization_variables(definition,definition.id);
        for(auto &parameter:definition.parameters)
            if(!parameter.default_expression.empty())
                parameter.value=evaluate_public_parameter_default(parameter,variables);
        definition_variables.emplace(definition.id,std::move(variables));
    }
    for(auto &definition:result.definitions) {
        Project body;
        static_cast<Schematic&>(body)=definition;
        body.id=definition.id;body.name=definition.name;body.profile=result.profile;
        body.definitions=result.definitions;
        resolve_schematic(body,definition_variables.at(definition.id));
        static_cast<Schematic&>(definition)=static_cast<const Schematic&>(body);
    }
    resolve_schematic(result,initialization_variables(result,result.id));
    return result;
}
} // namespace pds
