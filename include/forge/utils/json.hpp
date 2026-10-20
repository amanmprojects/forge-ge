#pragma once
// utils/json: dependency-free JSON value, parser and writer.
// - Header-only, no exceptions, no RTTI, no allocation in the parser beyond
//   the resulting tree.
// - Numbers are doubles (JSON has no int/float distinction); use asInt()/
//   asFloat()/asBool() for typed reads with defaults.
// - Object key order is preserved (Tiled relies on stable ordering for
//   diagnostics; writers emit insertion order).
// - Parsing is strict enough for hand-written files and Tiled exports:
//   comments and trailing commas are rejected, duplicate keys keep the last.
//
//   auto doc = forge::json::parse(text);
//   if (!doc) { /* doc.error() has line/column/message */ }
//   double w = doc->get("width").asFloat(0);
//   forge::json::Value out = forge::json::Value::object();
//   out.set("name", "hero");
//   std::string text = forge::json::write(out, true);

#include <string>
#include <string_view>
#include <vector>
#include <utility>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <charconv>

namespace forge::json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;
    explicit Value(bool b) : type_(Type::Bool), bool_(b) {}
    explicit Value(double n) : type_(Type::Number), num_(n) {}
    explicit Value(int n) : type_(Type::Number), num_((double)n) {}
    explicit Value(std::string s) : type_(Type::String), str_(std::move(s)) {}
    explicit Value(const char* s) : type_(Type::String), str_(s ? s : "") {}

    static Value array() { Value v; v.type_ = Type::Array; return v; }
    static Value object() { Value v; v.type_ = Type::Object; return v; }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    // ---- typed reads (never throw; defaults on type mismatch) ----
    bool asBool(bool def = false) const { return type_ == Type::Bool ? bool_ : def; }
    double asDouble(double def = 0.0) const { return type_ == Type::Number ? num_ : def; }
    float asFloat(float def = 0.0f) const {
        return type_ == Type::Number ? (float)num_ : def;
    }
    int asInt(int def = 0) const {
        return type_ == Type::Number ? (int)std::llround(num_) : def;
    }
    int64_t asInt64(int64_t def = 0) const {
        return type_ == Type::Number ? (int64_t)std::llround(num_) : def;
    }
    const std::string& asString() const {
        static const std::string empty;
        return type_ == Type::String ? str_ : empty;
    }
    std::string asString(const std::string& def) const {
        return type_ == Type::String ? str_ : def;
    }

    // ---- arrays ----
    size_t size() const {
        if (type_ == Type::Array) return arr_.size();
        if (type_ == Type::Object) return obj_.size();
        return 0;
    }
    bool empty() const { return size() == 0; }
    const Value& at(size_t i) const {
        static const Value null;
        return (type_ == Type::Array && i < arr_.size()) ? arr_[i] : null;
    }
    void push(Value v) {
        if (type_ != Type::Array) { type_ = Type::Array; arr_.clear(); }
        arr_.push_back(std::move(v));
    }

    // ---- objects ----
    const Value& get(std::string_view key) const {
        static const Value null;
        if (type_ != Type::Object) return null;
        for (auto& kv : obj_) if (kv.first == key) return kv.second;
        return null;
    }
    const Value& operator[](std::string_view key) const { return get(key); }
    bool has(std::string_view key) const {
        if (type_ != Type::Object) return false;
        for (auto& kv : obj_) if (kv.first == key) return true;
        return false;
    }
    void set(std::string key, Value v) {
        if (type_ != Type::Object) { type_ = Type::Object; obj_.clear(); }
        for (auto& kv : obj_) {
            if (kv.first == key) { kv.second = std::move(v); return; }
        }
        obj_.emplace_back(std::move(key), std::move(v));
    }
    // Insertion-ordered object entries.
    const std::vector<std::pair<std::string, Value>>& items() const { return obj_; }
    const std::vector<Value>& elements() const { return arr_; }

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    std::vector<Value> arr_;
    std::vector<std::pair<std::string, Value>> obj_;
};

// ------------------------------------------------------------------ parser
struct ParseError {
    int line = 0;
    int column = 0;
    std::string message;
    bool ok() const { return message.empty(); }
    std::string str() const {
        if (ok()) return "ok";
        return "line " + std::to_string(line) + ", column " +
               std::to_string(column) + ": " + message;
    }
};

namespace detail {

struct Parser {
    const char* p = nullptr;
    const char* begin = nullptr;
    ParseError err;
    int depth = 0;
    static constexpr int kMaxDepth = 200;

    void fail(const char* msg) {
        if (!err.ok()) return;
        err.message = msg;
        err.line = 1;
        err.column = 1;
        for (const char* q = begin; q < p && *q; ++q) {
            if (*q == '\n') { ++err.line; err.column = 1; }
            else ++err.column;
        }
    }

    void skipWs() {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    }

    bool parseValue(Value& out) {
        if (++depth > kMaxDepth) { fail("nesting too deep"); return false; }
        skipWs();
        bool ok = false;
        switch (*p) {
            case '{': ok = parseObject(out); break;
            case '[': ok = parseArray(out); break;
            case '"': {
                std::string s;
                ok = parseString(s);
                if (ok) out = Value(std::move(s));
                break;
            }
            case 't':
                if (!std::strncmp(p, "true", 4)) { p += 4; out = Value(true); ok = true; }
                else fail("invalid literal (expected true)");
                break;
            case 'f':
                if (!std::strncmp(p, "false", 5)) { p += 5; out = Value(false); ok = true; }
                else fail("invalid literal (expected false)");
                break;
            case 'n':
                if (!std::strncmp(p, "null", 4)) { p += 4; out = Value(); ok = true; }
                else fail("invalid literal (expected null)");
                break;
            case '\0': fail("unexpected end of input"); break;
            default: ok = parseNumber(out); break;
        }
        --depth;
        return ok;
    }

    bool parseObject(Value& out) {
        out = Value::object();
        ++p; // {
        skipWs();
        if (*p == '}') { ++p; return true; }
        while (*p) {
            skipWs();
            if (*p != '"') { fail("expected object key string"); return false; }
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (*p != ':') { fail("expected ':' after object key"); return false; }
            ++p;
            Value v;
            if (!parseValue(v)) return false;
            out.set(std::move(key), std::move(v));
            skipWs();
            if (*p == ',') { ++p; continue; }
            if (*p == '}') { ++p; return true; }
            fail("expected ',' or '}' in object");
            return false;
        }
        fail("unterminated object");
        return false;
    }

    bool parseArray(Value& out) {
        out = Value::array();
        ++p; // [
        skipWs();
        if (*p == ']') { ++p; return true; }
        while (*p) {
            Value v;
            if (!parseValue(v)) return false;
            out.push(std::move(v));
            skipWs();
            if (*p == ',') { ++p; continue; }
            if (*p == ']') { ++p; return true; }
            fail("expected ',' or ']' in array");
            return false;
        }
        fail("unterminated array");
        return false;
    }

    bool parseString(std::string& out) {
        ++p; // opening quote
        out.clear();
        while (*p && *p != '"') {
            unsigned char c = (unsigned char)*p;
            if (c == '\\') {
                ++p;
                switch (*p) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        // parseHex4 advances past the escape sequence itself,
                        // so skip the generic post-switch ++p below.
                        if (!parseHex4(out)) return false;
                        continue;
                    }
                    case '\0': fail("unterminated escape"); return false;
                    default: fail("invalid escape sequence"); return false;
                }
                if (*p) ++p;
            } else if (c < 0x20) {
                fail("control character in string");
                return false;
            } else {
                out += (char)c;
                ++p;
            }
        }
        if (*p != '"') { fail("unterminated string"); return false; }
        ++p;
        return true;
    }

    bool parseHex4(std::string& out) {
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        auto read4 = [&](uint32_t& cp) -> bool {
            cp = 0;
            for (int i = 0; i < 4; ++i) {
                ++p;
                int h = hex(*p);
                if (h < 0) { fail("invalid \\u escape"); return false; }
                cp = (cp << 4) | (uint32_t)h;
            }
            ++p;
            return true;
        };
        uint32_t cp = 0;
        if (!read4(cp)) return false;
        // Surrogate pair: \uD800-\uDBFF followed by \uDC00-\uDFFF.
        if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
            const char* save = p;
            ++p; // backslash
            uint32_t lo = 0;
            if (!read4(lo)) return false;
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else {
                p = save; // not a pair: emit the lone high surrogate as U+FFFD
                cp = 0xFFFD;
            }
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD;
        }
        appendUtf8(cp, out);
        return true;
    }

    static void appendUtf8(uint32_t cp, std::string& out) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }

    bool parseNumber(Value& out) {
        const char* start = p;
        if (*p == '-') ++p;
        if (*p == '0') {
            ++p;
        } else if (*p >= '1' && *p <= '9') {
            while (*p >= '0' && *p <= '9') ++p;
        } else {
            fail("invalid number");
            return false;
        }
        if (*p == '.') {
            ++p;
            if (!(*p >= '0' && *p <= '9')) { fail("invalid number (missing fraction)"); return false; }
            while (*p >= '0' && *p <= '9') ++p;
        }
        if (*p == 'e' || *p == 'E') {
            ++p;
            if (*p == '+' || *p == '-') ++p;
            if (!(*p >= '0' && *p <= '9')) { fail("invalid number (missing exponent)"); return false; }
            while (*p >= '0' && *p <= '9') ++p;
        }
        // strtod needs a NUL-terminated buffer; the caller guarantees that.
        char* end = nullptr;
        double d = std::strtod(start, &end);
        if (end != p) { fail("invalid number"); return false; }
        out = Value(d);
        return true;
    }
};

} // namespace detail

// Returns a null Value on failure; check `error` for the reason.
inline Value parse(std::string_view text, ParseError* error = nullptr) {
    detail::Parser parser;
    parser.p = text.data();
    parser.begin = text.data();
    Value out;
    bool ok = parser.parseValue(out);
    if (ok) {
        parser.skipWs();
        if (*parser.p != '\0') {
            parser.fail("trailing characters after JSON value");
            ok = false;
        }
    }
    if (error) *error = parser.err;
    return ok ? out : Value();
}

// Convenience: parse a file. Missing/unreadable files report an error.
inline Value parseFile(const std::string& path, ParseError* error = nullptr) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (error) { error->line = 0; error->column = 0; error->message = "cannot open file: " + path; }
        return Value();
    }
    std::fseek(f, 0, SEEK_END);
    long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len < 0) { std::fclose(f); if (error) error->message = "cannot stat file: " + path; return Value(); }
    std::string buf((size_t)len, '\0');
    size_t got = std::fread(buf.data(), 1, (size_t)len, f);
    std::fclose(f);
    if (got != (size_t)len) { if (error) error->message = "short read: " + path; return Value(); }
    return parse(buf, error);
}

// ------------------------------------------------------------------ writer
namespace detail {
inline void escape(const std::string& s, std::string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    out += '"';
}

inline void number(double d, std::string& out) {
    if (!std::isfinite(d)) { out += "0"; return; } // JSON has no inf/nan
    char buf[32];
    auto res = std::to_chars(buf, buf + sizeof(buf), d);
    out.append(buf, res.ptr);
}

inline void writeValue(const Value& v, std::string& out, bool pretty, int indent) {
    auto nl = [&](int depth) {
        if (!pretty) return;
        out += '\n';
        out.append((size_t)depth * 2, ' ');
    };
    switch (v.type()) {
        case Value::Type::Null: out += "null"; break;
        case Value::Type::Bool: out += v.asBool() ? "true" : "false"; break;
        case Value::Type::Number: number(v.asDouble(), out); break;
        case Value::Type::String: escape(v.asString(), out); break;
        case Value::Type::Array: {
            out += '[';
            auto& a = v.elements();
            for (size_t i = 0; i < a.size(); ++i) {
                if (i) out += ',';
                nl(indent + 1);
                writeValue(a[i], out, pretty, indent + 1);
            }
            if (!a.empty()) nl(indent);
            out += ']';
            break;
        }
        case Value::Type::Object: {
            out += '{';
            auto& o = v.items();
            for (size_t i = 0; i < o.size(); ++i) {
                if (i) out += ',';
                nl(indent + 1);
                escape(o[i].first, out);
                out += pretty ? ": " : ":";
                writeValue(o[i].second, out, pretty, indent + 1);
            }
            if (!o.empty()) nl(indent);
            out += '}';
            break;
        }
    }
}
} // namespace detail

inline std::string write(const Value& v, bool pretty = false) {
    std::string out;
    detail::writeValue(v, out, pretty, 0);
    return out;
}

// Writes atomically-ish: temp file then rename, so a crash never leaves a
// half-written save file. Returns false on I/O failure.
inline bool writeFile(const std::string& path, const Value& v, bool pretty = true) {
    std::string text = write(v, pretty);
    std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    size_t written = std::fwrite(text.data(), 1, text.size(), f);
    bool ok = written == text.size() && std::fclose(f) == 0;
    if (!ok) { std::remove(tmp.c_str()); return false; }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace forge::json
