#include "forge/script/script.hpp"
#include "forge/core/logger.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <unordered_set>

namespace forge::script {

std::string Error::str() const {
    if (ok()) return "ok";
    return "line " + std::to_string(line) + ", column " + std::to_string(column) +
           ": " + message;
}

std::string Value::toString() const {
    switch (type_) {
        case Type::Null: return "null";
        case Type::Bool: return bool_ ? "true" : "false";
        case Type::Number: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.15g", num_);
            return buf;
        }
        case Type::String: return str_;
        case Type::Table: {
            std::string out = "{";
            bool first = true;
            for (auto& kv : table_->items) {
                if (!first) out += ", ";
                first = false;
                out += kv.first + " = " + kv.second.toString();
            }
            return out + "}";
        }
    }
    return "null";
}

// =============================================================== lexer
namespace {

enum class Tok {
    End, Number, String, Ident,
    Var, Fn, If, Else, While, For, Return, Break, Continue,
    True, False, Null,
    LParen, RParen, LBrace, RBrace, Comma, Semi, Dot,
    Plus, Minus, Star, Slash, Percent,
    Assign, Eq, Ne, Lt, Le, Gt, Ge,
    AndAnd, OrOr, Bang,
};

struct Token {
    Tok kind = Tok::End;
    std::string text;
    double num = 0.0;
    int line = 1, col = 1;
};

struct Lexer {
    const char* p;
    const char* begin;
    int line = 1, col = 1;
    std::string err;
    int errLine = 0, errCol = 0;

    void advance() {
        if (*p == '\n') { ++line; col = 1; }
        else ++col;
        ++p;
    }
    char peek(int n = 0) const { return p[n]; }

    void fail(const std::string& m) {
        if (!err.empty()) return;
        err = m;
        errLine = line;
        errCol = col;
    }

    std::vector<Token> run() {
        std::vector<Token> out;
        while (*p) {
            // whitespace
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') advance();
            // comments: // and /* */
            if (*p == '/' && peek(1) == '/') {
                while (*p && *p != '\n') advance();
                continue;
            }
            if (*p == '/' && peek(1) == '*') {
                advance(); advance();
                while (*p && !(*p == '*' && peek(1) == '/')) advance();
                if (*p) { advance(); advance(); }
                else fail("unterminated block comment");
                continue;
            }
            if (!*p) break;
            Token t;
            t.line = line; t.col = col;
            char c = *p;
            if (c == '"' || c == '\'') {
                char quote = c;
                advance();
                std::string s;
                while (*p && *p != quote) {
                    if (*p == '\\') {
                        advance();
                        switch (*p) {
                            case 'n': s += '\n'; break;
                            case 't': s += '\t'; break;
                            case 'r': s += '\r'; break;
                            case '\\': s += '\\'; break;
                            case '"': s += '"'; break;
                            case '\'': s += '\''; break;
                            default: s += *p; break;
                        }
                        if (*p) advance();
                    } else {
                        s += *p;
                        advance();
                    }
                }
                if (*p != quote) { fail("unterminated string"); return out; }
                advance();
                t.kind = Tok::String;
                t.text = std::move(s);
                out.push_back(std::move(t));
                continue;
            }
            if ((c >= '0' && c <= '9') || (c == '.' && peek(1) >= '0' && peek(1) <= '9')) {
                const char* start = p;
                while (*p >= '0' && *p <= '9') advance();
                if (*p == '.') {
                    advance();
                    while (*p >= '0' && *p <= '9') advance();
                }
                if (*p == 'e' || *p == 'E') {
                    advance();
                    if (*p == '+' || *p == '-') advance();
                    while (*p >= '0' && *p <= '9') advance();
                }
                t.kind = Tok::Number;
                t.text.assign(start, (size_t)(p - start));
                t.num = std::strtod(t.text.c_str(), nullptr);
                out.push_back(std::move(t));
                continue;
            }
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
                const char* start = p;
                while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                       (*p >= '0' && *p <= '9') || *p == '_') advance();
                t.text.assign(start, (size_t)(p - start));
                if (t.text == "var") t.kind = Tok::Var;
                else if (t.text == "fn") t.kind = Tok::Fn;
                else if (t.text == "if") t.kind = Tok::If;
                else if (t.text == "else") t.kind = Tok::Else;
                else if (t.text == "while") t.kind = Tok::While;
                else if (t.text == "for") t.kind = Tok::For;
                else if (t.text == "return") t.kind = Tok::Return;
                else if (t.text == "break") t.kind = Tok::Break;
                else if (t.text == "continue") t.kind = Tok::Continue;
                else if (t.text == "true") t.kind = Tok::True;
                else if (t.text == "false") t.kind = Tok::False;
                else if (t.text == "null") t.kind = Tok::Null;
                else t.kind = Tok::Ident;
                out.push_back(std::move(t));
                continue;
            }
            auto two = [&](char a, char b) { return c == a && peek(1) == b; };
            if (two('=', '=')) { t.kind = Tok::Eq; advance(); advance(); }
            else if (two('!', '=')) { t.kind = Tok::Ne; advance(); advance(); }
            else if (two('<', '=')) { t.kind = Tok::Le; advance(); advance(); }
            else if (two('>', '=')) { t.kind = Tok::Ge; advance(); advance(); }
            else if (two('&', '&')) { t.kind = Tok::AndAnd; advance(); advance(); }
            else if (two('|', '|')) { t.kind = Tok::OrOr; advance(); advance(); }
            else {
                switch (c) {
                    case '(': t.kind = Tok::LParen; break;
                    case ')': t.kind = Tok::RParen; break;
                    case '{': t.kind = Tok::LBrace; break;
                    case '}': t.kind = Tok::RBrace; break;
                    case ',': t.kind = Tok::Comma; break;
                    case ';': t.kind = Tok::Semi; break;
                    case '.': t.kind = Tok::Dot; break;
                    case '+': t.kind = Tok::Plus; break;
                    case '-': t.kind = Tok::Minus; break;
                    case '*': t.kind = Tok::Star; break;
                    case '/': t.kind = Tok::Slash; break;
                    case '%': t.kind = Tok::Percent; break;
                    case '=': t.kind = Tok::Assign; break;
                    case '<': t.kind = Tok::Lt; break;
                    case '>': t.kind = Tok::Gt; break;
                    case '!': t.kind = Tok::Bang; break;
                    default: fail(std::string("unexpected character '") + c + "'"); return out;
                }
                advance();
            }
            out.push_back(std::move(t));
        }
        Token end;
        end.kind = Tok::End;
        end.line = line; end.col = col;
        out.push_back(end);
        return out;
    }
};

// =============================================================== AST
struct Expr;
struct Stmt;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

struct Expr {
    enum class Kind {
        Number, String, Bool, Null, Ident, Unary, Binary, Call, Member, Assign
    } kind = Kind::Null;
    double num = 0;
    std::string text;   // ident / string / member / operator
    bool boolean = false;
    ExprPtr a, b;
    std::vector<ExprPtr> list; // call args
    int line = 0, col = 0;
};

struct Stmt {
    enum class Kind {
        Var, Fn, If, While, For, Return, Break, Continue, Expr, Block
    } kind = Kind::Expr;
    std::string name;
    ExprPtr expr;             // var init / condition / return value
    ExprPtr expr2;            // for: condition
    StmtPtr init;             // for: initializer
    StmtPtr step;             // for: increment
    std::vector<StmtPtr> body;
    std::vector<StmtPtr> elseBody;
    std::vector<std::string> params;
    int line = 0, col = 0;
};

struct Function {
    std::string name;
    std::vector<std::string> params;
    std::vector<StmtPtr> body;
};

// =============================================================== parser
struct Parser {
    std::vector<Token> toks;
    size_t i = 0;
    std::string err;
    int errLine = 0, errCol = 0;

    const Token& cur() const { return toks[i]; }
    const Token& peek(int n = 1) const {
        size_t k = std::min(i + (size_t)n, toks.size() - 1);
        return toks[k];
    }
    bool at(Tok k) const { return cur().kind == k; }
    bool accept(Tok k) {
        if (at(k)) { ++i; return true; }
        return false;
    }
    void fail(const std::string& m) {
        if (!err.empty()) return;
        err = m;
        errLine = cur().line;
        errCol = cur().col;
    }
    bool expect(Tok k, const char* what) {
        if (accept(k)) return true;
        fail(std::string("expected ") + what);
        return false;
    }

    StmtPtr parseStatement();
    std::vector<StmtPtr> parseBlock();
    ExprPtr parseExpr();
    ExprPtr parseAssign();
    ExprPtr parseOr();
    ExprPtr parseAnd();
    ExprPtr parseEquality();
    ExprPtr parseComparison();
    ExprPtr parseTerm();
    ExprPtr parseFactor();
    ExprPtr parseUnary();
    ExprPtr parsePostfix();
    ExprPtr parsePrimary();
    bool parseProgram(std::vector<StmtPtr>& out, std::vector<Function>& fns);
};

ExprPtr Parser::parsePrimary() {
    ExprPtr e = std::make_unique<Expr>();
    e->line = cur().line; e->col = cur().col;
    if (accept(Tok::Number)) {
        e->kind = Expr::Kind::Number;
        e->num = toks[i - 1].num;
        return e;
    }
    if (accept(Tok::String)) {
        e->kind = Expr::Kind::String;
        e->text = toks[i - 1].text;
        return e;
    }
    if (accept(Tok::True)) { e->kind = Expr::Kind::Bool; e->boolean = true; return e; }
    if (accept(Tok::False)) { e->kind = Expr::Kind::Bool; e->boolean = false; return e; }
    if (accept(Tok::Null)) { e->kind = Expr::Kind::Null; return e; }
    if (accept(Tok::Ident)) {
        e->kind = Expr::Kind::Ident;
        e->text = toks[i - 1].text;
        return e;
    }
    if (accept(Tok::LParen)) {
        ExprPtr inner = parseExpr();
        if (!expect(Tok::RParen, "')'")) return nullptr;
        return inner;
    }
    fail("expected expression");
    return nullptr;
}

ExprPtr Parser::parsePostfix() {
    ExprPtr e = parsePrimary();
    if (!e) return nullptr;
    for (;;) {
        if (accept(Tok::LParen)) {
            ExprPtr call = std::make_unique<Expr>();
            call->kind = Expr::Kind::Call;
            call->line = cur().line; call->col = cur().col;
            call->a = std::move(e);
            if (!at(Tok::RParen)) {
                for (;;) {
                    ExprPtr arg = parseExpr();
                    if (!arg) return nullptr;
                    call->list.push_back(std::move(arg));
                    if (!accept(Tok::Comma)) break;
                }
            }
            if (!expect(Tok::RParen, "')' after call arguments")) return nullptr;
            e = std::move(call);
            continue;
        }
        if (accept(Tok::Dot)) {
            if (!at(Tok::Ident)) { fail("expected member name after '.'"); return nullptr; }
            ExprPtr m = std::make_unique<Expr>();
            m->kind = Expr::Kind::Member;
            m->line = cur().line; m->col = cur().col;
            m->text = cur().text;
            ++i;
            m->a = std::move(e);
            e = std::move(m);
            continue;
        }
        break;
    }
    return e;
}

ExprPtr Parser::parseUnary() {
    if (at(Tok::Minus) || at(Tok::Bang)) {
        Tok op = cur().kind;
        ExprPtr e = std::make_unique<Expr>();
        e->kind = Expr::Kind::Unary;
        e->line = cur().line; e->col = cur().col;
        e->text = (op == Tok::Minus) ? "-" : "!";
        ++i;
        e->a = parseUnary();
        return e;
    }
    return parsePostfix();
}

ExprPtr Parser::parseFactor() {
    ExprPtr e = parseUnary();
    while (e && (at(Tok::Star) || at(Tok::Slash) || at(Tok::Percent))) {
        Tok op = cur().kind;
        ExprPtr b = std::make_unique<Expr>();
        b->kind = Expr::Kind::Binary;
        b->text = op == Tok::Star ? "*" : (op == Tok::Slash ? "/" : "%");
        b->line = cur().line; b->col = cur().col;
        ++i;
        b->a = std::move(e);
        b->b = parseUnary();
        if (!b->b) return nullptr;
        e = std::move(b);
    }
    return e;
}

ExprPtr Parser::parseTerm() {
    ExprPtr e = parseFactor();
    while (e && (at(Tok::Plus) || at(Tok::Minus))) {
        Tok op = cur().kind;
        ExprPtr b = std::make_unique<Expr>();
        b->kind = Expr::Kind::Binary;
        b->text = op == Tok::Plus ? "+" : "-";
        b->line = cur().line; b->col = cur().col;
        ++i;
        b->a = std::move(e);
        b->b = parseFactor();
        if (!b->b) return nullptr;
        e = std::move(b);
    }
    return e;
}

ExprPtr Parser::parseComparison() {
    ExprPtr e = parseTerm();
    while (e && (at(Tok::Lt) || at(Tok::Le) || at(Tok::Gt) || at(Tok::Ge))) {
        Tok op = cur().kind;
        ExprPtr b = std::make_unique<Expr>();
        b->kind = Expr::Kind::Binary;
        b->text = op == Tok::Lt ? "<" : op == Tok::Le ? "<=" : op == Tok::Gt ? ">" : ">=";
        b->line = cur().line; b->col = cur().col;
        ++i;
        b->a = std::move(e);
        b->b = parseTerm();
        if (!b->b) return nullptr;
        e = std::move(b);
    }
    return e;
}

ExprPtr Parser::parseEquality() {
    ExprPtr e = parseComparison();
    while (e && (at(Tok::Eq) || at(Tok::Ne))) {
        Tok op = cur().kind;
        ExprPtr b = std::make_unique<Expr>();
        b->kind = Expr::Kind::Binary;
        b->text = op == Tok::Eq ? "==" : "!=";
        b->line = cur().line; b->col = cur().col;
        ++i;
        b->a = std::move(e);
        b->b = parseComparison();
        if (!b->b) return nullptr;
        e = std::move(b);
    }
    return e;
}

ExprPtr Parser::parseAnd() {
    ExprPtr e = parseEquality();
    while (e && at(Tok::AndAnd)) {
        ExprPtr b = std::make_unique<Expr>();
        b->kind = Expr::Kind::Binary;
        b->text = "&&";
        b->line = cur().line; b->col = cur().col;
        ++i;
        b->a = std::move(e);
        b->b = parseEquality();
        if (!b->b) return nullptr;
        e = std::move(b);
    }
    return e;
}

ExprPtr Parser::parseOr() {
    ExprPtr e = parseAnd();
    while (e && at(Tok::OrOr)) {
        ExprPtr b = std::make_unique<Expr>();
        b->kind = Expr::Kind::Binary;
        b->text = "||";
        b->line = cur().line; b->col = cur().col;
        ++i;
        b->a = std::move(e);
        b->b = parseAnd();
        if (!b->b) return nullptr;
        e = std::move(b);
    }
    return e;
}

ExprPtr Parser::parseAssign() {
    ExprPtr e = parseOr();
    if (e && at(Tok::Assign)) {
        ExprPtr b = std::make_unique<Expr>();
        b->kind = Expr::Kind::Assign;
        b->line = cur().line; b->col = cur().col;
        ++i;
        b->a = std::move(e);
        b->b = parseAssign(); // right-associative
        return b;
    }
    return e;
}

ExprPtr Parser::parseExpr() { return parseAssign(); }

std::vector<StmtPtr> Parser::parseBlock() {
    std::vector<StmtPtr> out;
    if (!expect(Tok::LBrace, "'{'")) return out;
    while (!at(Tok::RBrace) && !at(Tok::End)) {
        StmtPtr s = parseStatement();
        if (!s) return out;
        out.push_back(std::move(s));
    }
    expect(Tok::RBrace, "'}'");
    return out;
}

StmtPtr Parser::parseStatement() {
    StmtPtr s = std::make_unique<Stmt>();
    s->line = cur().line; s->col = cur().col;

    if (accept(Tok::Var)) {
        s->kind = Stmt::Kind::Var;
        if (!at(Tok::Ident)) { fail("expected variable name"); return nullptr; }
        s->name = cur().text;
        ++i;
        if (accept(Tok::Assign)) {
            s->expr = parseExpr();
            if (!s->expr) return nullptr;
        }
        accept(Tok::Semi);
        return s;
    }
    if (accept(Tok::If)) {
        s->kind = Stmt::Kind::If;
        if (!expect(Tok::LParen, "'(' after if")) return nullptr;
        s->expr = parseExpr();
        if (!s->expr) return nullptr;
        if (!expect(Tok::RParen, "')' after condition")) return nullptr;
        s->body = parseBlock();
        if (!err.empty()) return nullptr;
        if (accept(Tok::Else)) {
            if (at(Tok::If)) {
                StmtPtr nested = parseStatement();
                if (!nested) return nullptr;
                s->elseBody.push_back(std::move(nested));
            } else {
                s->elseBody = parseBlock();
                if (!err.empty()) return nullptr;
            }
        }
        return s;
    }
    if (accept(Tok::While)) {
        s->kind = Stmt::Kind::While;
        if (!expect(Tok::LParen, "'(' after while")) return nullptr;
        s->expr = parseExpr();
        if (!s->expr) return nullptr;
        if (!expect(Tok::RParen, "')' after condition")) return nullptr;
        s->body = parseBlock();
        if (!err.empty()) return nullptr;
        return s;
    }
    if (accept(Tok::For)) {
        s->kind = Stmt::Kind::For;
        if (!expect(Tok::LParen, "'(' after for")) return nullptr;
        if (!at(Tok::Semi)) {
            s->init = parseStatement();
            if (!s->init) return nullptr;
        } else {
            ++i;
        }
        if (!at(Tok::Semi)) {
            s->expr = parseExpr();
            if (!s->expr) return nullptr;
        }
        if (!expect(Tok::Semi, "';' in for")) return nullptr;
        if (!at(Tok::RParen)) {
            s->step = std::make_unique<Stmt>();
            s->step->kind = Stmt::Kind::Expr;
            s->step->expr = parseExpr();
            if (!s->step->expr) return nullptr;
        }
        if (!expect(Tok::RParen, "')' after for")) return nullptr;
        s->body = parseBlock();
        if (!err.empty()) return nullptr;
        return s;
    }
    if (accept(Tok::Return)) {
        s->kind = Stmt::Kind::Return;
        if (!at(Tok::Semi) && !at(Tok::RBrace)) {
            s->expr = parseExpr();
            if (!s->expr) return nullptr;
        }
        accept(Tok::Semi);
        return s;
    }
    if (accept(Tok::Break)) { s->kind = Stmt::Kind::Break; accept(Tok::Semi); return s; }
    if (accept(Tok::Continue)) { s->kind = Stmt::Kind::Continue; accept(Tok::Semi); return s; }
    if (at(Tok::LBrace)) {
        s->kind = Stmt::Kind::Block;
        s->body = parseBlock();
        if (!err.empty()) return nullptr;
        return s;
    }
    s->kind = Stmt::Kind::Expr;
    s->expr = parseExpr();
    if (!s->expr) return nullptr;
    accept(Tok::Semi);
    return s;
}

bool Parser::parseProgram(std::vector<StmtPtr>& out, std::vector<Function>& fns) {
    while (!at(Tok::End)) {
        if (accept(Tok::Fn)) {
            Function f;
            if (!at(Tok::Ident)) { fail("expected function name"); return false; }
            f.name = cur().text;
            ++i;
            if (!expect(Tok::LParen, "'(' after function name")) return false;
            if (!at(Tok::RParen)) {
                for (;;) {
                    if (!at(Tok::Ident)) { fail("expected parameter name"); return false; }
                    f.params.push_back(cur().text);
                    ++i;
                    if (!accept(Tok::Comma)) break;
                }
            }
            if (!expect(Tok::RParen, "')' after parameters")) return false;
            f.body = parseBlock();
            if (!err.empty()) return false;
            fns.push_back(std::move(f));
            continue;
        }
        StmtPtr s = parseStatement();
        if (!s) return false;
        out.push_back(std::move(s));
    }
    return true;
}

} // namespace

// =============================================================== interpreter
struct ScriptVM::Impl {
    std::vector<StmtPtr> program;
    std::vector<Function> functions;
    std::unordered_map<std::string, Value> globals;
    std::unordered_map<std::string, HostFn> host;
    bool initialized = false;
};

namespace {

struct ReturnSignal { Value value; };
struct BreakSignal {};
struct ContinueSignal {};
struct RuntimeError {
    std::string message;
    int line = 0, col = 0;
};

} // namespace

struct Interp {
    ScriptVM& vm;
    ScriptVM::Impl& im;
    std::unordered_map<std::string, Value>* locals = nullptr;
    uint64_t steps = 0;
    uint64_t budget = 1000000;

    void tick(int line, int col) {
        if (++steps > budget) {
            throw RuntimeError{"step budget exceeded (infinite loop?)", line, col};
        }
    }

    Value* findVar(const std::string& name) {
        if (locals) {
            auto it = locals->find(name);
            if (it != locals->end()) return &it->second;
        }
        auto it = im.globals.find(name);
        if (it != im.globals.end()) return &it->second;
        return nullptr;
    }

    Value eval(Expr& e);
    void exec(Stmt& s);
    void execBlock(std::vector<StmtPtr>& body);
    Value callFunction(const std::string& name, const std::vector<Value>& args);
};

Value Interp::eval(Expr& e) {
    switch (e.kind) {
        case Expr::Kind::Number: return Value(e.num);
        case Expr::Kind::String: return Value(e.text);
        case Expr::Kind::Bool: return Value(e.boolean);
        case Expr::Kind::Null: return Value();
        case Expr::Kind::Ident: {
            Value* v = findVar(e.text);
            if (!v) throw RuntimeError{"undefined variable '" + e.text + "'", e.line, e.col};
            return *v;
        }
        case Expr::Kind::Member: {
            Value base = eval(*e.a);
            if (!base.isTable()) {
                // Member read on null yields null (lets `if (self.foo)` work
                // without a table existing yet).
                if (base.isNull()) return Value();
                throw RuntimeError{"member access on non-table value", e.line, e.col};
            }
            return base.get(e.text);
        }
        case Expr::Kind::Unary: {
            Value v = eval(*e.a);
            if (e.text == "-") return Value(-v.number());
            return Value(!v.boolean());
        }
        case Expr::Kind::Binary: {
            // Short-circuit before evaluating the right side.
            if (e.text == "&&") {
                Value a = eval(*e.a);
                if (!a.boolean()) return Value(false);
                return Value(eval(*e.b).boolean());
            }
            if (e.text == "||") {
                Value a = eval(*e.a);
                if (a.boolean()) return Value(true);
                return Value(eval(*e.b).boolean());
            }
            Value a = eval(*e.a);
            Value b = eval(*e.b);
            if (e.text == "+") {
                if (a.isString() || b.isString())
                    return Value(a.toString() + b.toString());
                return Value(a.number() + b.number());
            }
            if (e.text == "-") return Value(a.number() - b.number());
            if (e.text == "*") return Value(a.number() * b.number());
            if (e.text == "/") {
                double d = b.number();
                if (d == 0.0) throw RuntimeError{"division by zero", e.line, e.col};
                return Value(a.number() / d);
            }
            if (e.text == "%") {
                double d = b.number();
                if (d == 0.0) throw RuntimeError{"modulo by zero", e.line, e.col};
                return Value(std::fmod(a.number(), d));
            }
            if (e.text == "==") {
                if (a.isNumber() && b.isNumber()) return Value(a.number() == b.number());
                if (a.isBool() && b.isBool()) return Value(a.boolean() == b.boolean());
                if (a.isString() && b.isString()) return Value(a.string() == b.string());
                return Value(a.type() == b.type() && a.type() == Value::Type::Null);
            }
            if (e.text == "!=") {
                if (a.isNumber() && b.isNumber()) return Value(a.number() != b.number());
                if (a.isString() && b.isString()) return Value(a.string() != b.string());
                if (a.isBool() && b.isBool()) return Value(a.boolean() != b.boolean());
                return Value(a.type() != b.type());
            }
            if (e.text == "<") return Value(a.number() < b.number());
            if (e.text == "<=") return Value(a.number() <= b.number());
            if (e.text == ">") return Value(a.number() > b.number());
            if (e.text == ">=") return Value(a.number() >= b.number());
            throw RuntimeError{"unknown operator '" + e.text + "'", e.line, e.col};
        }
        case Expr::Kind::Call: {
            // Direct function name: host binding or script function.
            if (e.a->kind == Expr::Kind::Ident) {
                const std::string& name = e.a->text;
                std::vector<Value> args;
                args.reserve(e.list.size());
                for (auto& arg : e.list) args.push_back(eval(*arg));
                auto hit = im.host.find(name);
                if (hit != im.host.end()) {
                    try {
                        return hit->second(args);
                    } catch (const RuntimeError&) {
                        throw;
                    } catch (const std::exception& ex) {
                        throw RuntimeError{"host function '" + name + "' failed: " + ex.what(),
                                           e.line, e.col};
                    }
                }
                for (auto& f : im.functions)
                    if (f.name == name) return callFunction(name, args);
                throw RuntimeError{"unknown function '" + name + "'", e.line, e.col};
            }
            throw RuntimeError{"only named functions can be called", e.line, e.col};
        }
        case Expr::Kind::Assign: {
            Value v = eval(*e.b);
            if (e.a->kind == Expr::Kind::Ident) {
                Value* slot = findVar(e.a->text);
                if (!slot) {
                    // Assignment to an unknown name declares it globally; this
                    // keeps small scripts terse without a `var` ceremony.
                    im.globals[e.a->text] = v;
                } else {
                    *slot = v;
                }
                return v;
            }
            if (e.a->kind == Expr::Kind::Member) {
                Value base = eval(*e.a->a);
                if (!base.isTable()) {
                    // Auto-vivify tables for `self.x = ...` when self is null.
                    if (base.isNull()) throw RuntimeError{
                        "assignment to member of null (create the table first)",
                        e.line, e.col};
                    throw RuntimeError{"member assignment on non-table", e.line, e.col};
                }
                base.set(e.a->text, v);
                return v;
            }
            throw RuntimeError{"invalid assignment target", e.line, e.col};
        }
    }
    return Value();
}

void Interp::execBlock(std::vector<StmtPtr>& body) {
    for (auto& s : body) exec(*s);
}

void Interp::exec(Stmt& s) {
    tick(s.line, s.col);
    switch (s.kind) {
        case Stmt::Kind::Var: {
            Value v = s.expr ? eval(*s.expr) : Value();
            if (locals) (*locals)[s.name] = v;
            else im.globals[s.name] = v;
            return;
        }
        case Stmt::Kind::Expr:
            if (s.expr) eval(*s.expr);
            return;
        case Stmt::Kind::If: {
            if (eval(*s.expr).boolean()) execBlock(s.body);
            else if (!s.elseBody.empty()) execBlock(s.elseBody);
            return;
        }
        case Stmt::Kind::While: {
            while (eval(*s.expr).boolean()) {
                tick(s.line, s.col);
                try {
                    execBlock(s.body);
                } catch (const ContinueSignal&) {
                    continue;
                } catch (const BreakSignal&) {
                    break;
                }
            }
            return;
        }
        case Stmt::Kind::For: {
            if (s.init) exec(*s.init);
            while (!s.expr || eval(*s.expr).boolean()) {
                tick(s.line, s.col);
                try {
                    execBlock(s.body);
                } catch (const ContinueSignal&) {
                    // fall through to the step
                } catch (const BreakSignal&) {
                    break;
                }
                if (s.step) exec(*s.step);
            }
            return;
        }
        case Stmt::Kind::Return:
            throw ReturnSignal{s.expr ? eval(*s.expr) : Value()};
        case Stmt::Kind::Break:
            throw BreakSignal{};
        case Stmt::Kind::Continue:
            throw ContinueSignal{};
        case Stmt::Kind::Block:
            execBlock(s.body);
            return;
        case Stmt::Kind::Fn:
            return; // top-level fn is collected by the parser
    }
}

Value Interp::callFunction(const std::string& name, const std::vector<Value>& args) {
    Function* fn = nullptr;
    for (auto& f : im.functions)
        if (f.name == name) { fn = &f; break; }
    if (!fn) throw RuntimeError{"unknown function '" + name + "'", 0, 0};
    if (args.size() != fn->params.size()) {
        throw RuntimeError{"'" + name + "' expects " +
                           std::to_string(fn->params.size()) + " argument(s), got " +
                           std::to_string(args.size()), 0, 0};
    }
    std::unordered_map<std::string, Value> frame;
    for (size_t i = 0; i < fn->params.size(); ++i) frame[fn->params[i]] = args[i];
    auto* prev = locals;
    locals = &frame;
    try {
        execBlock(fn->body);
    } catch (const ReturnSignal& r) {
        locals = prev;
        return r.value;
    } catch (...) {
        locals = prev;
        throw;
    }
    locals = prev;
    return Value();
}

// =============================================================== ScriptVM
ScriptVM::ScriptVM() : impl_(std::make_unique<Impl>()) {
    // Built-ins available to every script.
    impl_->host["print"] = [this](const std::vector<Value>& a) {
        std::string out;
        for (size_t i = 0; i < a.size(); ++i) {
            if (i) out += " ";
            out += a[i].toString();
        }
        if (printSink_) printSink_(out);
        else FORGE_LOG_INFO("%s", out.c_str());
        return Value();
    };
    impl_->host["abs"] = [](const std::vector<Value>& a) {
        return Value(std::fabs(a.empty() ? 0.0 : a[0].number()));
    };
    impl_->host["min"] = [](const std::vector<Value>& a) {
        if (a.empty()) return Value(0.0);
        double v = a[0].number();
        for (size_t i = 1; i < a.size(); ++i) v = std::min(v, a[i].number());
        return Value(v);
    };
    impl_->host["max"] = [](const std::vector<Value>& a) {
        if (a.empty()) return Value(0.0);
        double v = a[0].number();
        for (size_t i = 1; i < a.size(); ++i) v = std::max(v, a[i].number());
        return Value(v);
    };
    impl_->host["floor"] = [](const std::vector<Value>& a) {
        return Value(std::floor(a.empty() ? 0.0 : a[0].number()));
    };
    impl_->host["ceil"] = [](const std::vector<Value>& a) {
        return Value(std::ceil(a.empty() ? 0.0 : a[0].number()));
    };
    impl_->host["clamp"] = [](const std::vector<Value>& a) {
        if (a.size() < 3) return Value(0.0);
        return Value(std::clamp(a[0].number(), a[1].number(), a[2].number()));
    };
    impl_->host["sqrt"] = [](const std::vector<Value>& a) {
        return Value(std::sqrt(std::max(0.0, a.empty() ? 0.0 : a[0].number())));
    };
    impl_->host["table"] = [](const std::vector<Value>&) { return Value::table(); };
    impl_->host["str"] = [](const std::vector<Value>& a) {
        return Value(a.empty() ? std::string() : a[0].toString());
    };
}

ScriptVM::~ScriptVM() = default;

bool ScriptVM::loadFile(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        error_ = {0, 0, "cannot open script: " + path};
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string src;
    if (len > 0) {
        src.resize((size_t)len);
        size_t got = std::fread(src.data(), 1, (size_t)len, f);
        src.resize(got);
    }
    std::fclose(f);
    return loadSource(src, path);
}

bool ScriptVM::loadSource(const std::string& source, const std::string& name) {
    error_ = {};
    sourceName_ = name;
    impl_->program.clear();
    impl_->functions.clear();
    impl_->initialized = false;

    Lexer lex;
    lex.p = source.c_str();
    lex.begin = source.c_str();
    std::vector<Token> toks = lex.run();
    if (!lex.err.empty()) {
        error_ = {lex.errLine, lex.errCol, lex.err};
        return false;
    }
    Parser parser;
    parser.toks = std::move(toks);
    std::vector<Function> fns;
    if (!parser.parseProgram(impl_->program, fns)) {
        error_ = {parser.errLine, parser.errCol, parser.err};
        return false;
    }
    impl_->functions = std::move(fns);
    return true;
}

void ScriptVM::clear() {
    impl_->program.clear();
    impl_->functions.clear();
    impl_->globals.clear();
    impl_->initialized = false;
    sourceName_.clear();
    error_ = {};
}

void ScriptVM::setGlobal(const std::string& name, Value v) {
    impl_->globals[name] = std::move(v);
}

Value ScriptVM::getGlobal(const std::string& name) const {
    auto it = impl_->globals.find(name);
    return it == impl_->globals.end() ? Value() : it->second;
}

bool ScriptVM::hasGlobal(const std::string& name) const {
    return impl_->globals.count(name) != 0;
}

void ScriptVM::bind(const std::string& name, HostFn fn) {
    impl_->host[name] = std::move(fn);
}

void ScriptVM::unbind(const std::string& name) { impl_->host.erase(name); }

bool ScriptVM::hasFunction(const std::string& name) const {
    for (auto& f : impl_->functions) if (f.name == name) return true;
    return false;
}

std::vector<std::string> ScriptVM::functions() const {
    std::vector<std::string> out;
    for (auto& f : impl_->functions) out.push_back(f.name);
    return out;
}

Value ScriptVM::call(const std::string& function, const std::vector<Value>& args) {
    error_ = {};
    stepsUsed_ = 0;
    Interp interp{*this, *impl_};
    interp.budget = stepBudget_;
    try {
        // Top-level statements are initialization: run them exactly once,
        // lazily, on the first call (so globals can be configured first).
        if (!impl_->initialized) {
            impl_->initialized = true;
            Interp init{*this, *impl_};
            init.budget = stepBudget_;
            for (auto& s : impl_->program) init.exec(*s);
            stepsUsed_ = init.steps;
        }
        Value v = interp.callFunction(function, args);
        stepsUsed_ += interp.steps;
        return v;
    } catch (const ReturnSignal& r) {
        return r.value;
    } catch (const RuntimeError& e) {
        error_ = {e.line, e.col, e.message};
        stepsUsed_ += interp.steps;
        return Value();
    } catch (const std::exception& e) {
        error_ = {0, 0, std::string("runtime error: ") + e.what()};
        return Value();
    }
}

} // namespace forge::script
