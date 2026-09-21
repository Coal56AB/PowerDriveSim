#include "core/model/c_program.hpp"
#include "core/model/model.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <set>
#include <utility>
#include <vector>

namespace pds {
namespace {

[[noreturn]] void fail(const CProgramOptions &options, const std::string &message) {
    throw Diagnostic(options.diagnostic_code, options.object, message);
}

struct Token {
    enum class Kind { identifier, number, symbol, end } kind = Kind::end;
    std::string text;
    double number = 0;
    std::size_t line = 1;
    std::size_t column = 1;
};

std::vector<Token> lex(const std::string &source, const CProgramOptions &options) {
    if(source.size()>1024*1024)fail(options,"C program exceeds 1 MiB");
    std::vector<Token> result;
    std::size_t i = 0, line = 1, column = 1;
    auto advance = [&](char c) { if (c == '\n') { ++line; column = 1; } else ++column; ++i; };
    auto error = [&](const std::string &message) -> void {
        fail(options, message + " at line " + std::to_string(line) + ", column " + std::to_string(column));
    };
    while (i < source.size()) {
        if(result.size()>200000)error("C program contains too many tokens");
        const auto c = static_cast<unsigned char>(source[i]);
        if (std::isspace(c)) { advance(source[i]); continue; }
        if (source[i] == '#') error("Preprocessor directives are not available");
        if (i + 1 < source.size() && source[i] == '/' && source[i + 1] == '/') {
            while (i < source.size() && source[i] != '\n') advance(source[i]);
            continue;
        }
        if (i + 1 < source.size() && source[i] == '/' && source[i + 1] == '*') {
            advance('/'); advance('*');
            while (i + 1 < source.size() && !(source[i] == '*' && source[i + 1] == '/')) advance(source[i]);
            if (i + 1 >= source.size()) error("Unterminated block comment");
            advance('*'); advance('/'); continue;
        }
        const auto token_line = line, token_column = column;
        if (std::isalpha(c) || source[i] == '_') {
            const auto begin = i;
            while (i < source.size()) {
                const auto d = static_cast<unsigned char>(source[i]);
                if (!std::isalnum(d) && source[i] != '_') break;
                advance(source[i]);
            }
            result.push_back({Token::Kind::identifier, source.substr(begin, i - begin), 0, token_line, token_column});
            continue;
        }
        if (std::isdigit(c) || (source[i] == '.' && i + 1 < source.size() && std::isdigit(static_cast<unsigned char>(source[i + 1])))) {
            const char *begin = source.c_str() + i;
            char *end = nullptr;
            const double value = std::strtod(begin, &end);
            if (end == begin || !std::isfinite(value)) error("Invalid numeric literal");
            const auto used = static_cast<std::size_t>(end - begin);
            const auto text = source.substr(i, used);
            for (std::size_t n = 0; n < used; ++n) advance(source[i]);
            while (i < source.size() && (source[i] == 'f' || source[i] == 'F' || source[i] == 'l' || source[i] == 'L' || source[i] == 'u' || source[i] == 'U')) advance(source[i]);
            result.push_back({Token::Kind::number, text, value, token_line, token_column});
            continue;
        }
        static const std::vector<std::string> operators = {
            "<<=", ">>=", "++", "--", "+=", "-=", "*=", "/=", "%=", "==", "!=",
            "<=", ">=", "&&", "||", "<<", ">>", "&=", "|=", "^=", "->"
        };
        bool matched = false;
        for (const auto &op : operators) if (source.compare(i, op.size(), op) == 0) {
            result.push_back({Token::Kind::symbol, op, 0, token_line, token_column});
            for (std::size_t n = 0; n < op.size(); ++n) advance(source[i]);
            matched = true; break;
        }
        if (matched) continue;
        if (std::string("+-*/%=!<>&|^~?:;,(){}").find(source[i]) == std::string::npos)
            error("Unsupported character");
        result.push_back({Token::Kind::symbol, std::string(1, source[i]), 0, token_line, token_column});
        advance(source[i]);
    }
    result.push_back({Token::Kind::end, {}, 0, line, column});
    return result;
}

struct Expression {
    enum class Kind { number, variable, unary, binary, conditional, call, assignment, prefix, postfix } kind;
    std::string text;
    double number = 0;
    std::vector<std::unique_ptr<Expression>> children;
};

struct Declaration {
    std::string name;
    std::unique_ptr<Expression> initializer;
    bool constant = false;
    bool persistent = false;
    std::size_t identity = 0;
};

struct Statement {
    enum class Kind { empty, expression, declaration, block, if_statement, while_statement,
                      do_statement, for_statement, return_statement, break_statement, continue_statement } kind;
    std::vector<Declaration> declarations;
    std::vector<std::unique_ptr<Statement>> statements;
    std::unique_ptr<Expression> first, second, third;
    std::unique_ptr<Statement> body, alternative;
};

struct Function {
    std::vector<std::string> parameters;
    std::unique_ptr<Statement> body;
};

bool type_name(const std::string &name) {
    static const std::set<std::string> names = {"bool", "char", "short", "int", "long", "float", "double", "auto", "void"};
    return names.contains(name);
}

class SyntaxParser {
public:
    SyntaxParser(std::vector<Token> tokens, CProgramOptions options)
        : tokens_(std::move(tokens)), options_(std::move(options)) {}

    void parse(std::vector<std::unique_ptr<Statement>> &statements, std::map<std::string, Function> &functions) {
        while (!at_end()) {
            const auto saved = position_;
            bool constant = false, persistent = false;
            consume_qualifiers(constant, persistent);
            if (peek().kind == Token::Kind::identifier && type_name(peek().text)) {
                take();
                if (peek().kind == Token::Kind::identifier && peek(1).text == "(") {
                    const auto name = take().text;
                    take();
                    Function function;
                    if (!match(")")) {
                        do {
                            bool ignored_const = false, ignored_static = false;
                            consume_qualifiers(ignored_const, ignored_static);
                            if (!type_name(expect_identifier("Expected parameter type").text)) error("Expected parameter type");
                            function.parameters.push_back(expect_identifier("Expected parameter name").text);
                        } while (match(","));
                        expect(")");
                    }
                    if (functions.contains(name)) error("Duplicate function '" + name + "'");
                    function.body = parse_statement();
                    if (function.body->kind != Statement::Kind::block) error("Function body must be a block");
                    functions.emplace(name, std::move(function));
                    continue;
                }
            }
            position_ = saved;
            statements.push_back(parse_statement());
        }
    }

private:
    struct Depth { std::size_t &value; ~Depth(){--value;} };
    const Token &peek(std::size_t offset = 0) const { return tokens_[std::min(position_ + offset, tokens_.size() - 1)]; }
    Token take() { return tokens_[position_++]; }
    bool at_end() const { return peek().kind == Token::Kind::end; }
    bool match(const std::string &text) { if (peek().text != text) return false; ++position_; return true; }
    void expect(const std::string &text) { if (!match(text)) error("Expected '" + text + "'"); }
    Token expect_identifier(const std::string &message) {
        if (peek().kind != Token::Kind::identifier) error(message);
        return take();
    }
    [[noreturn]] void error(const std::string &message) const {
        fail(options_, message + " at line " + std::to_string(peek().line) + ", column " + std::to_string(peek().column));
    }
    void consume_qualifiers(bool &constant, bool &persistent) {
        for (;;) {
            if (match("const")) constant = true;
            else if (match("static")) persistent = true;
            else if (match("volatile") || match("signed") || match("unsigned")) {}
            else break;
        }
    }
    bool declaration_ahead() const {
        std::size_t p = position_;
        while (p < tokens_.size() && (tokens_[p].text == "const" || tokens_[p].text == "static" ||
               tokens_[p].text == "volatile" || tokens_[p].text == "signed" || tokens_[p].text == "unsigned")) ++p;
        return p < tokens_.size() && tokens_[p].kind == Token::Kind::identifier && type_name(tokens_[p].text);
    }
    std::unique_ptr<Statement> parse_declaration(bool semicolon = true) {
        auto statement = std::make_unique<Statement>(); statement->kind = Statement::Kind::declaration;
        bool constant = false, persistent = false; consume_qualifiers(constant, persistent);
        const auto type = expect_identifier("Expected a C scalar type").text;
        if (!type_name(type) || type == "void") error("Expected a non-void C scalar type");
        do {
            Declaration declaration;
            declaration.name = expect_identifier("Expected variable name").text;
            declaration.constant = constant; declaration.persistent = persistent; declaration.identity = next_declaration_++;
            if (match("=")) declaration.initializer = expression();
            statement->declarations.push_back(std::move(declaration));
        } while (match(","));
        if (semicolon) expect(";");
        return statement;
    }
    std::unique_ptr<Statement> parse_statement() {
        if(++statement_depth_>256)error("C statement nesting limit exceeded");Depth statement_guard{statement_depth_};
        if (match(";")) { auto s=std::make_unique<Statement>();s->kind=Statement::Kind::empty;return s; }
        if (match("{")) {
            auto s=std::make_unique<Statement>();s->kind=Statement::Kind::block;
            while (!match("}")) { if(at_end())error("Expected '}'"); s->statements.push_back(parse_statement()); }
            return s;
        }
        if (match("if")) {
            auto s=std::make_unique<Statement>();s->kind=Statement::Kind::if_statement;expect("(");s->first=expression();expect(")");s->body=parse_statement();
            if(match("else"))s->alternative=parse_statement();return s;
        }
        if (match("while")) {
            auto s=std::make_unique<Statement>();s->kind=Statement::Kind::while_statement;expect("(");s->first=expression();expect(")");s->body=parse_statement();return s;
        }
        if (match("do")) {
            auto s=std::make_unique<Statement>();s->kind=Statement::Kind::do_statement;s->body=parse_statement();
            if(!match("while"))error("Expected 'while'");expect("(");s->first=expression();expect(")");expect(";");return s;
        }
        if (match("for")) {
            auto s=std::make_unique<Statement>();s->kind=Statement::Kind::for_statement;expect("(");
            if(declaration_ahead())s->statements.push_back(parse_declaration());
            else { auto init=std::make_unique<Statement>();init->kind=Statement::Kind::expression;if(!match(";")){init->first=expression();expect(";");}s->statements.push_back(std::move(init)); }
            if(!match(";")){s->first=expression();expect(";");}
            if(!match(")")){s->second=expression();expect(")");}
            s->body=parse_statement();return s;
        }
        if (match("return")) { auto s=std::make_unique<Statement>();s->kind=Statement::Kind::return_statement;if(!match(";")){s->first=expression();expect(";");}return s; }
        if (match("break")) { expect(";");auto s=std::make_unique<Statement>();s->kind=Statement::Kind::break_statement;return s; }
        if (match("continue")) { expect(";");auto s=std::make_unique<Statement>();s->kind=Statement::Kind::continue_statement;return s; }
        if (declaration_ahead()) return parse_declaration();
        auto s=std::make_unique<Statement>();s->kind=Statement::Kind::expression;s->first=expression();expect(";");return s;
    }
    std::unique_ptr<Expression> expression() { return assignment(); }
    std::unique_ptr<Expression> assignment() {
        if(++expression_depth_>256)error("C expression nesting limit exceeded");Depth expression_guard{expression_depth_};
        auto left=conditional();
        static const std::set<std::string> ops={"=","+=","-=","*=","/=","%=","&=","|=","^=","<<=",">>="};
        if(ops.contains(peek().text)){auto op=take().text;auto right=assignment();return node(Expression::Kind::assignment,op,std::move(left),std::move(right));}
        return left;
    }
    std::unique_ptr<Expression> conditional() {
        if(++conditional_depth_>256)error("C conditional nesting limit exceeded");Depth conditional_guard{conditional_depth_};
        auto value=logical_or();if(!match("?"))return value;
        auto yes=expression();expect(":");auto no=conditional();auto n=std::make_unique<Expression>();n->kind=Expression::Kind::conditional;n->children.push_back(std::move(value));n->children.push_back(std::move(yes));n->children.push_back(std::move(no));return n;
    }
#define PDS_BINARY(name,next,...) std::unique_ptr<Expression> name(){auto v=next();static const std::set<std::string> ops={__VA_ARGS__};while(ops.contains(peek().text)){auto op=take().text;v=node(Expression::Kind::binary,op,std::move(v),next());}return v;}
    PDS_BINARY(logical_or,logical_and,"||")
    PDS_BINARY(logical_and,bitwise_or,"&&")
    PDS_BINARY(bitwise_or,bitwise_xor,"|")
    PDS_BINARY(bitwise_xor,bitwise_and,"^")
    PDS_BINARY(bitwise_and,equality,"&")
    PDS_BINARY(equality,comparison,"==","!=")
    PDS_BINARY(comparison,shift,"<",">","<=",">=")
    PDS_BINARY(shift,addition,"<<",">>")
    PDS_BINARY(addition,multiplication,"+","-")
    PDS_BINARY(multiplication,unary,"*","/","%")
#undef PDS_BINARY
    std::unique_ptr<Expression> unary() {
        if(++unary_depth_>256)error("C unary nesting limit exceeded");Depth unary_guard{unary_depth_};
        if(cast_ahead()) {
            expect("(");bool ignored_const=false,ignored_static=false;consume_qualifiers(ignored_const,ignored_static);
            const auto type=expect_identifier("Expected cast type").text;
            while(peek().kind==Token::Kind::identifier&&(peek().text=="long"||peek().text=="short"||peek().text=="int"))take();
            expect(")");auto n=std::make_unique<Expression>();n->kind=Expression::Kind::unary;
            n->text=type=="bool"?"cast_bool":(type=="float"||type=="double"?"cast_real":"cast_integer");n->children.push_back(unary());return n;
        }
        static const std::set<std::string> ops={"+","-","!","~","++","--"};
        if(ops.contains(peek().text)){auto op=take().text;auto n=std::make_unique<Expression>();n->kind=(op=="++"||op=="--")?Expression::Kind::prefix:Expression::Kind::unary;n->text=op;n->children.push_back(unary());return n;}
        return postfix();
    }
    bool cast_ahead() const {
        if(peek().text!="(")return false;std::size_t p=position_+1;
        while(p<tokens_.size()&&(tokens_[p].text=="const"||tokens_[p].text=="volatile"||tokens_[p].text=="signed"||tokens_[p].text=="unsigned"))++p;
        return p<tokens_.size()&&tokens_[p].kind==Token::Kind::identifier&&type_name(tokens_[p].text);
    }
    std::unique_ptr<Expression> postfix() {
        auto v=primary();if(peek().text=="++"||peek().text=="--"){auto n=std::make_unique<Expression>();n->kind=Expression::Kind::postfix;n->text=take().text;n->children.push_back(std::move(v));return n;}return v;
    }
    std::unique_ptr<Expression> primary() {
        if(match("(")){auto v=expression();expect(")");return v;}
        if(peek().kind==Token::Kind::number){auto n=std::make_unique<Expression>();n->kind=Expression::Kind::number;n->number=take().number;return n;}
        const auto name=expect_identifier("Expected expression").text;
        if(match("(")){auto n=std::make_unique<Expression>();n->kind=Expression::Kind::call;n->text=name;if(!match(")")){do n->children.push_back(expression());while(match(","));expect(")");}return n;}
        auto n=std::make_unique<Expression>();n->kind=Expression::Kind::variable;n->text=name;return n;
    }
    std::unique_ptr<Expression> node(Expression::Kind kind,std::string text,std::unique_ptr<Expression> a,std::unique_ptr<Expression> b) {
        auto n=std::make_unique<Expression>();n->kind=kind;n->text=std::move(text);n->children.push_back(std::move(a));n->children.push_back(std::move(b));return n;
    }
    std::vector<Token> tokens_; CProgramOptions options_; std::size_t position_=0, next_declaration_=1;
    std::size_t statement_depth_=0,expression_depth_=0,conditional_depth_=0,unary_depth_=0;
};

class SemanticValidator {
public:
    SemanticValidator(const CProgramOptions &options,const std::map<std::string,Function> &functions)
        : options_(options),functions_(functions) {}
    void validate(const std::vector<std::unique_ptr<Statement>> &statements) {
        scopes_.emplace_back();scopes_.back()["true"]=true;scopes_.back()["false"]=true;
        for(const auto *constant:{"M_PI","PI","M_E","E"})scopes_.back()[constant]=true;
        if(options_.allow_time){scopes_.back()["t"]=true;scopes_.back()["stime"]=true;}
        for(const auto &name:options_.writable_variables)
            if(!options_.external_variables.contains(name))error("Writable variable '"+name+"' is not external");
        for(const auto &name:options_.external_variables) {
            if(name.empty())error("External variable name must not be empty");
            declare(name,!options_.writable_variables.contains(name));
        }
        for(const auto &statement:statements)if(statement->kind==Statement::Kind::declaration)
            for(const auto &declaration:statement->declarations)declare(declaration.name,declaration.constant);
        for(const auto &[name,function]:functions_) {
            (void)name;scopes_.emplace_back();
            for(const auto &parameter:function.parameters)declare(parameter,false);
            statement(*function.body,0);scopes_.pop_back();
        }
        for(const auto &entry:statements)statement(*entry,0);
        if(options_.require_return&&!saw_return_)error("C program must contain return");
    }
private:
    [[noreturn]] void error(const std::string &message) const { fail(options_,message); }
    void declare(const std::string &name,bool constant) {
        if(scopes_.back().contains(name))error("Duplicate variable '"+name+"'");
        scopes_.back()[name]=constant;
    }
    bool lookup(const std::string &name,bool *constant=nullptr) const {
        for(auto scope=scopes_.rbegin();scope!=scopes_.rend();++scope)if(auto found=scope->find(name);found!=scope->end()){if(constant)*constant=found->second;return true;}return false;
    }
    static std::optional<std::size_t> builtin_arity(const std::string &name) {
        static const std::map<std::string,std::size_t> arities={{"abs",1},{"fabs",1},{"sqrt",1},{"sin",1},{"cos",1},{"tan",1},{"asin",1},{"acos",1},{"atan",1},{"atan2",2},{"exp",1},{"log",1},{"log10",1},{"floor",1},{"ceil",1},{"round",1},{"pow",2},{"fmod",2},{"min",2},{"fmin",2},{"max",2},{"fmax",2},{"clamp",3},{"ramp",4},{"pwm",3},{"square",3},{"phasepwm",3}};
        if(auto found=arities.find(name);found!=arities.end())return found->second;return {};
    }
    void expression(const Expression &value) {
        if(value.kind==Expression::Kind::variable&&!lookup(value.text))error("Unknown variable '"+value.text+"'");
        if(value.kind==Expression::Kind::assignment||value.kind==Expression::Kind::prefix||value.kind==Expression::Kind::postfix) {
            const auto &target=*value.children.front();bool constant=false;
            if(target.kind!=Expression::Kind::variable)error("Assignment target must be a variable");
            if(!lookup(target.text,&constant))error("Unknown variable '"+target.text+"'");
            if(constant)error("Cannot modify const variable '"+target.text+"'");
        }
        if(value.kind==Expression::Kind::call) {
            if(auto arity=builtin_arity(value.text)) {
                if(*arity!=value.children.size())error("Invalid argument count for '"+value.text+"'");
                if((value.text=="ramp"||value.text=="pwm"||value.text=="square"||value.text=="phasepwm")&&!options_.allow_gate_functions)
                    error("Gate function '"+value.text+"' is not available here");
            } else if(auto found=functions_.find(value.text);found==functions_.end())error("Unknown function '"+value.text+"'");
            else if(found->second.parameters.size()!=value.children.size())error("Invalid argument count for '"+value.text+"'");
        }
        for(const auto &child:value.children)expression(*child);
    }
    void statement(const Statement &value,int loop_depth) {
        switch(value.kind) {
        case Statement::Kind::empty:break;
        case Statement::Kind::expression:if(value.first)expression(*value.first);break;
        case Statement::Kind::declaration:
            for(const auto &declaration:value.declarations){if(declaration.initializer)expression(*declaration.initializer);if(scopes_.size()>1)declare(declaration.name,declaration.constant);}break;
        case Statement::Kind::block:
            scopes_.emplace_back();for(const auto &child:value.statements)statement(*child,loop_depth);scopes_.pop_back();break;
        case Statement::Kind::if_statement:
            expression(*value.first);statement(*value.body,loop_depth);if(value.alternative)statement(*value.alternative,loop_depth);break;
        case Statement::Kind::while_statement:
        case Statement::Kind::do_statement:
            expression(*value.first);statement(*value.body,loop_depth+1);break;
        case Statement::Kind::for_statement:
            scopes_.emplace_back();statement(*value.statements.front(),loop_depth+1);if(value.first)expression(*value.first);if(value.second)expression(*value.second);statement(*value.body,loop_depth+1);scopes_.pop_back();break;
        case Statement::Kind::return_statement:saw_return_=true;if(value.first)expression(*value.first);break;
        case Statement::Kind::break_statement:
        case Statement::Kind::continue_statement:if(loop_depth==0)error("break/continue used outside a loop");break;
        }
    }
    const CProgramOptions &options_;const std::map<std::string,Function> &functions_;
    std::vector<std::map<std::string,bool>> scopes_;bool saw_return_=false;
};

struct Binding { double value=0; bool constant=false; std::optional<std::size_t> persistent; };
enum class Flow { normal, returned, break_loop, continue_loop };
struct Outcome { Flow flow=Flow::normal; double value=0; };

class Runtime {
public:
    Runtime(const CProgramOptions &options,const std::map<std::string,Function> &functions,double time,
            CProgramState *state,const std::map<std::string,double> &inputs)
        : options_(options),functions_(functions),time_(time),state_(state) {
        scopes_.emplace_back();
        for(const auto &[name,value]:inputs)
            if(!options_.external_variables.contains(name))error("Undeclared external variable '"+name+"'");
        for(const auto &name:options_.external_variables) {
            const auto found=inputs.find(name);
            scopes_.back().emplace(name,Binding{found==inputs.end()?0.0:found->second,
                                               !options_.writable_variables.contains(name),{}});
        }
        if(options.allow_time){scopes_.back().emplace("t",Binding{time,true,{}});scopes_.back().emplace("stime",Binding{time,true,{}});}
        scopes_.back().emplace("true",Binding{1,true,{}});scopes_.back().emplace("false",Binding{0,true,{}});
        scopes_.back().emplace("M_PI",Binding{3.1415926535897932384626433832795,true,{}});
        scopes_.back().emplace("PI",Binding{3.1415926535897932384626433832795,true,{}});
        scopes_.back().emplace("M_E",Binding{2.7182818284590452353602874713527,true,{}});
        scopes_.back().emplace("E",Binding{2.7182818284590452353602874713527,true,{}});
    }
    CProgramResult run(const std::vector<std::unique_ptr<Statement>> &statements) {
        Outcome outcome;
        for(const auto &statement:statements){outcome=execute(*statement);if(outcome.flow!=Flow::normal)break;}
        if(outcome.flow==Flow::break_loop||outcome.flow==Flow::continue_loop)error("break/continue used outside a loop");
        CProgramResult result;if(outcome.flow==Flow::returned)result.return_value=outcome.value;
        if(options_.require_return&&!result.return_value)error("C program must return a value");
        if(result.return_value&&!std::isfinite(*result.return_value))error("C program return value must be finite");
        for(const auto &[name,binding]:scopes_.front())if(name!="t"&&name!="stime"&&name!="true"&&name!="false")result.variables[name]=binding.value;
        return result;
    }
private:
    [[noreturn]] void error(const std::string &message) const { fail(options_,message); }
    void tick(){if(++instructions_>options_.instruction_budget)error("C program instruction budget exceeded");}
    Binding &binding(const std::string &name) {
        for(auto scope=scopes_.rbegin();scope!=scopes_.rend();++scope)if(auto found=scope->find(name);found!=scope->end())return found->second;
        error("Unknown variable '"+name+"'");
    }
    long long integer(double value) const {
        if(!std::isfinite(value)||value<double(std::numeric_limits<long long>::min())||
           value>=double(std::numeric_limits<long long>::max()))error("Integer conversion is out of range");
        return static_cast<long long>(value);
    }
    unsigned shift(double value) const {
        const auto count=integer(value);
        if(count<0||count>=64)error("Shift count must be in range 0..63");
        return static_cast<unsigned>(count);
    }
    double assign(const Expression &target,double value,const std::string &op="=") {
        if(target.kind!=Expression::Kind::variable)error("Assignment target must be a variable");
        auto &b=binding(target.text);if(b.constant)error("Cannot modify const variable '"+target.text+"'");
        double next=value;
        if(op!="="){
            const auto old=b.value;
            if(op=="+=")next=old+value;else if(op=="-=")next=old-value;else if(op=="*=")next=old*value;
            else if(op=="/="){if(value==0)error("Division by zero");next=old/value;}
            else if(op=="%="){if(value==0)error("Division by zero");next=std::fmod(old,value);}
            else if(op=="&=")next=double(integer(old)&integer(value));else if(op=="|=")next=double(integer(old)|integer(value));
            else if(op=="^=")next=double(integer(old)^integer(value));else if(op=="<<=")next=double(static_cast<unsigned long long>(integer(old))<<shift(value));
            else if(op==">>=")next=double(static_cast<unsigned long long>(integer(old))>>shift(value));
        }
        if(!std::isfinite(next))error("C program result must be finite");b.value=next;
        if(b.persistent&&state_)state_->static_values[*b.persistent]=next;return next;
    }
    double evaluate(const Expression &e) {
        tick();
        switch(e.kind){
        case Expression::Kind::number:return e.number;
        case Expression::Kind::variable:return binding(e.text).value;
        case Expression::Kind::unary:{const auto v=evaluate(*e.children[0]);if(e.text=="+")return v;if(e.text=="-")return -v;if(e.text=="!")return v==0;if(e.text=="~")return double(~integer(v));if(e.text=="cast_bool")return v!=0;if(e.text=="cast_integer")return double(integer(v));if(e.text=="cast_real")return v;break;}
        case Expression::Kind::prefix:{auto &target=*e.children[0];const auto v=binding(target.text).value+(e.text=="++"?1:-1);return assign(target,v);}
        case Expression::Kind::postfix:{auto &target=*e.children[0];const auto old=binding(target.text).value;assign(target,old+(e.text=="++"?1:-1));return old;}
        case Expression::Kind::assignment:return assign(*e.children[0],evaluate(*e.children[1]),e.text);
        case Expression::Kind::conditional:return evaluate(*e.children[evaluate(*e.children[0])!=0?1:2]);
        case Expression::Kind::call:{std::vector<double>a;for(const auto &child:e.children)a.push_back(evaluate(*child));return call(e.text,a);}
        case Expression::Kind::binary:{
            const auto left=evaluate(*e.children[0]);if(e.text=="&&")return left!=0&&evaluate(*e.children[1])!=0;if(e.text=="||")return left!=0||evaluate(*e.children[1])!=0;
            const auto right=evaluate(*e.children[1]);if(e.text=="+")return left+right;if(e.text=="-")return left-right;if(e.text=="*")return left*right;
            if(e.text=="/"){if(right==0)error("Division by zero");return left/right;}if(e.text=="%"){if(right==0)error("Division by zero");return std::fmod(left,right);}
            if(e.text=="==")return left==right;if(e.text=="!=")return left!=right;if(e.text=="<")return left<right;if(e.text==">")return left>right;if(e.text=="<=")return left<=right;if(e.text==">=")return left>=right;
            if(e.text=="&")return double(integer(left)&integer(right));if(e.text=="|")return double(integer(left)|integer(right));if(e.text=="^")return double(integer(left)^integer(right));if(e.text=="<<")return double(static_cast<unsigned long long>(integer(left))<<shift(right));if(e.text==">>")return double(static_cast<unsigned long long>(integer(left))>>shift(right));break;}
        }
        error("Unsupported C expression");
    }
    double call(const std::string &name,const std::vector<double> &a) {
        auto count=[&](std::size_t n){if(a.size()!=n)error("Invalid argument count for '"+name+"'");};
        if(name=="abs"||name=="fabs"){count(1);return std::abs(a[0]);}if(name=="sqrt"){count(1);if(a[0]<0)error("sqrt domain error");return std::sqrt(a[0]);}
        if(name=="sin"){count(1);return std::sin(a[0]);}if(name=="cos"){count(1);return std::cos(a[0]);}if(name=="tan"){count(1);return std::tan(a[0]);}
        if(name=="asin"){count(1);return std::asin(a[0]);}if(name=="acos"){count(1);return std::acos(a[0]);}if(name=="atan"){count(1);return std::atan(a[0]);}if(name=="atan2"){count(2);return std::atan2(a[0],a[1]);}
        if(name=="exp"){count(1);return std::exp(a[0]);}if(name=="log"){count(1);return std::log(a[0]);}if(name=="log10"){count(1);return std::log10(a[0]);}
        if(name=="floor"){count(1);return std::floor(a[0]);}if(name=="ceil"){count(1);return std::ceil(a[0]);}if(name=="round"){count(1);return std::round(a[0]);}
        if(name=="pow"){count(2);return std::pow(a[0],a[1]);}if(name=="fmod"){count(2);if(a[1]==0)error("Division by zero");return std::fmod(a[0],a[1]);}
        if(name=="min"||name=="fmin"){count(2);return std::min(a[0],a[1]);}if(name=="max"||name=="fmax"){count(2);return std::max(a[0],a[1]);}
        if(name=="clamp"){count(3);if(a[1]>a[2])error("clamp minimum exceeds maximum");return std::clamp(a[0],a[1],a[2]);}
        if(name=="ramp"){if(!options_.allow_gate_functions)error("Function 'ramp' is not available here");count(4);if(a[1]<=a[0])error("Ramp end time must exceed start time");const auto k=std::clamp((time_-a[0])/(a[1]-a[0]),0.0,1.0);return a[2]+(a[3]-a[2])*k;}
        if(name=="pwm"||name=="square"||name=="phasepwm"){
            if(!options_.allow_gate_functions)error("Gate functions are not available here");count(3);if(a[0]<=0||a[1]<0||a[1]>1||(name!="phasepwm"&&a[2]<0))error("Invalid PWM arguments");if(a[1]==0)return 0;if(a[1]==1)return 1;
            const auto period=1.0/a[0];double delay=name=="phasepwm"?std::fmod(a[2],period):a[2];if(delay<0)delay+=period;if(name!="phasepwm"&&time_<delay)return 0;double phase=std::fmod(time_-delay,period);if(phase<0)phase+=period;return phase<a[1]*period;}
        const auto found=functions_.find(name);if(found==functions_.end())error("Unknown function '"+name+"'");if(found->second.parameters.size()!=a.size())error("Invalid argument count for '"+name+"'");
        if(++call_depth_>options_.call_depth_limit)error("C function call depth exceeded");scopes_.emplace_back();for(std::size_t i=0;i<a.size();++i)scopes_.back().emplace(found->second.parameters[i],Binding{a[i],false,{}});
        const auto result=execute(*found->second.body);scopes_.pop_back();--call_depth_;if(result.flow==Flow::break_loop||result.flow==Flow::continue_loop)error("break/continue escaped a function");return result.flow==Flow::returned?result.value:0;
    }
    Outcome execute(const Statement &s) {
        tick();switch(s.kind){
        case Statement::Kind::empty:return {};
        case Statement::Kind::expression:if(s.first)(void)evaluate(*s.first);return {};
        case Statement::Kind::declaration:for(const auto &d:s.declarations){if(scopes_.back().contains(d.name))error("Duplicate variable '"+d.name+"'");double value=0;if(d.persistent&&state_&&state_->initialized[d.identity])value=state_->static_values[d.identity];else {if(d.initializer)value=evaluate(*d.initializer);if(d.persistent&&state_){state_->initialized[d.identity]=true;state_->static_values[d.identity]=value;}}scopes_.back().emplace(d.name,Binding{value,d.constant,d.persistent?std::optional<std::size_t>(d.identity):std::nullopt});}return {};
        case Statement::Kind::block:{scopes_.emplace_back();for(const auto &child:s.statements){auto out=execute(*child);if(out.flow!=Flow::normal){scopes_.pop_back();return out;}}scopes_.pop_back();return {};}
        case Statement::Kind::if_statement:if(evaluate(*s.first)!=0)return execute(*s.body);else if(s.alternative)return execute(*s.alternative);return {};
        case Statement::Kind::while_statement:while(evaluate(*s.first)!=0){auto out=execute(*s.body);if(out.flow==Flow::returned)return out;if(out.flow==Flow::break_loop)break;}return {};
        case Statement::Kind::do_statement:do{auto out=execute(*s.body);if(out.flow==Flow::returned)return out;if(out.flow==Flow::break_loop)break;}while(evaluate(*s.first)!=0);return {};
        case Statement::Kind::for_statement:{scopes_.emplace_back();(void)execute(*s.statements[0]);while(!s.first||evaluate(*s.first)!=0){auto out=execute(*s.body);if(out.flow==Flow::returned){scopes_.pop_back();return out;}if(out.flow==Flow::break_loop)break;if(s.second)(void)evaluate(*s.second);}scopes_.pop_back();return {};}
        case Statement::Kind::return_statement:return {Flow::returned,s.first?evaluate(*s.first):0};
        case Statement::Kind::break_statement:return {Flow::break_loop,0};case Statement::Kind::continue_statement:return {Flow::continue_loop,0};}
        return {};
    }
    const CProgramOptions &options_;const std::map<std::string,Function> &functions_;double time_;CProgramState *state_;
    std::vector<std::map<std::string,Binding>> scopes_;std::size_t instructions_=0,call_depth_=0;
};
} // namespace

struct CProgram::Implementation {
    CProgramOptions options;
    std::vector<std::unique_ptr<Statement>> statements;
    std::map<std::string, Function> functions;
};

CProgram compile_c_program(const std::string &source,const CProgramOptions &options) {
    try {
        auto implementation=std::make_shared<CProgram::Implementation>();implementation->options=options;
        SyntaxParser parser(lex(source,options),options);parser.parse(implementation->statements,implementation->functions);
        SemanticValidator(options,implementation->functions).validate(implementation->statements);
        return CProgram(std::move(implementation));
    } catch(const Diagnostic &) { throw; }
    catch(const std::exception &error) { fail(options,std::string("C parser failure: ")+error.what()); }
    catch(...) { fail(options,"Unknown C parser failure"); }
}

CProgramResult execute_c_program(const CProgram &program,double time,CProgramState *state,const std::map<std::string,double> &inputs) {
    if(!program.implementation_)throw Diagnostic("invalid_c_program","","C program is not compiled");
    const auto &implementation=*program.implementation_;
    try { Runtime runtime(implementation.options,implementation.functions,time,state,inputs);return runtime.run(implementation.statements); }
    catch(const Diagnostic &) { throw; }
    catch(const std::exception &error) { fail(implementation.options,std::string("C runtime failure: ")+error.what()); }
    catch(...) { fail(implementation.options,"Unknown C runtime failure"); }
}

} // namespace pds
