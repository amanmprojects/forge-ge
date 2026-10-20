#pragma once
// forge::script — a small, deterministic, dependency-free scripting language.
//
// Why not Lua: Forge's promise is "one static library, zero system
// dependencies". Lua would be a vendored C dependency plus a binding layer;
// this interpreter is a few hundred lines of C++20, has no allocation
// surprises, and is deterministic (no hash-order iteration, step budget
// instead of watchdogs).
//
// Language (deliberately small):
//
//   var speed = 120;
//   var jumps = 0;
//   fn on_update(dt) {
//       if (self.left) { self.x = self.x - speed * dt; }
//       if (self.jump && self.on_ground) { self.vy = 420; jumps = jumps + 1; }
//       while (jumps > 3) { jumps = jumps - 1; }
//       return jumps;
//   }
//
// Types: number (double), bool, string, null, table. Tables are ordered
// string->Value maps with shared_ptr semantics, so `self.x`, config objects
// and arrays-of-sorts work without a separate array type.
//
// Host integration:
//   ScriptVM vm;
//   vm.setGlobal("speed", 120.0);
//   vm.bind("spawn", [](const std::vector<Value>& a) { ... return Value(); });
//   vm.loadFile("scripts/player.fgs");
//   vm.call("on_update", {Value(dt)});
//
// Errors never throw: parse/compile errors are reported with line/column and
// runtime errors abort the current call (the VM stays usable). Every call has
// a step budget (default 1,000,000) so a bad loop cannot hang the game.

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <utility>
#include <cstdint>

namespace forge::script {

struct Table;
class Value;

class Value {
public:
    enum class Type { Null, Number, Bool, String, Table };

    Value() = default;
    Value(double n) : type_(Type::Number), num_(n) {}
    Value(int n) : type_(Type::Number), num_((double)n) {}
    Value(float n) : type_(Type::Number), num_((double)n) {}
    Value(bool b) : type_(Type::Bool), bool_(b) {}
    Value(const char* s) : type_(Type::String), str_(s ? s : "") {}
    Value(std::string s) : type_(Type::String), str_(std::move(s)) {}
    explicit Value(std::shared_ptr<Table> t) : type_(Type::Table), table_(std::move(t)) {}

    static Value table();

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isString() const { return type_ == Type::String; }
    bool isTable() const { return type_ == Type::Table; }

    double number(double def = 0.0) const { return type_ == Type::Number ? num_ : def; }
    bool boolean(bool def = false) const {
        if (type_ == Type::Bool) return bool_;
        if (type_ == Type::Number) return num_ != 0.0;
        if (type_ == Type::String) return !str_.empty();
        if (type_ == Type::Table) return true;
        return def;
    }
    const std::string& string() const {
        static const std::string empty;
        return type_ == Type::String ? str_ : empty;
    }

    // Table access (null when not a table / missing key).
    Value get(const std::string& key) const;
    void set(const std::string& key, Value v);
    std::shared_ptr<Table> tablePtr() const { return table_; }
    const std::vector<std::pair<std::string, Value>>& entries() const;

    // Human-readable form for print()/error messages.
    std::string toString() const;

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    std::shared_ptr<Table> table_;
};

struct Table {
    std::vector<std::pair<std::string, Value>> items;

    const Value* find(const std::string& key) const {
        for (auto& kv : items) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    Value* find(const std::string& key) {
        for (auto& kv : items) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    void set(const std::string& key, Value v) {
        if (Value* existing = find(key)) { *existing = std::move(v); return; }
        items.emplace_back(key, std::move(v));
    }
    const std::vector<std::pair<std::string, Value>>& entries() const { return items; }
};

inline Value Value::table() { return Value(std::make_shared<Table>()); }

inline Value Value::get(const std::string& key) const {
    if (!table_) return Value();
    const Value* v = table_->find(key);
    return v ? *v : Value();
}

inline void Value::set(const std::string& key, Value v) {
    if (!table_) { table_ = std::make_shared<Table>(); type_ = Type::Table; }
    table_->set(key, std::move(v));
}

inline const std::vector<std::pair<std::string, Value>>& Value::entries() const {
    static const std::vector<std::pair<std::string, Value>> empty;
    return table_ ? table_->entries() : empty;
}

struct Error {
    int line = 0;
    int column = 0;
    std::string message;
    bool ok() const { return message.empty(); }
    std::string str() const;
};

using HostFn = std::function<Value(const std::vector<Value>&)>;

class ScriptVM {
public:
    ScriptVM();
    ~ScriptVM();
    ScriptVM(const ScriptVM&) = delete;
    ScriptVM& operator=(const ScriptVM&) = delete;

    // ---- loading ----
    bool loadFile(const std::string& path);
    bool loadSource(const std::string& source, const std::string& name = "<memory>");
    void clear(); // drops functions but keeps host bindings/globals
    bool loaded() const { return !sourceName_.empty(); }
    const std::string& sourceName() const { return sourceName_; }

    // ---- host interface ----
    void setGlobal(const std::string& name, Value v);
    Value getGlobal(const std::string& name) const;
    bool hasGlobal(const std::string& name) const;
    void bind(const std::string& name, HostFn fn);
    void unbind(const std::string& name);
    void printTo(std::function<void(const std::string&)> sink) { printSink_ = std::move(sink); }

    // Calls a function defined by the script. Returns null on error (check
    // error()). Missing functions are an error.
    Value call(const std::string& function, const std::vector<Value>& args = {});
    bool hasFunction(const std::string& name) const;
    std::vector<std::string> functions() const;

    // ---- diagnostics ----
    const Error& error() const { return error_; }
    void clearError() { error_ = {}; }
    // Steps allowed per call (loops are the only way to exceed it).
    void setStepBudget(uint64_t steps) { stepBudget_ = steps; }
    uint64_t stepBudget() const { return stepBudget_; }
    uint64_t lastStepCount() const { return stepsUsed_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string sourceName_;
    Error error_;
    std::function<void(const std::string&)> printSink_;
    uint64_t stepBudget_ = 1000000;
    uint64_t stepsUsed_ = 0;

    friend struct Interp;
};

// Convenience for C++ callers: build an argument list in one expression.
inline std::vector<Value> args() { return {}; }
template <typename... Ts>
std::vector<Value> args(Ts&&... vs) {
    return std::vector<Value>{Value(std::forward<Ts>(vs))...};
}

} // namespace forge::script
