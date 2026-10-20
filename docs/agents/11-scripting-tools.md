# 11 — Scripting, JSON and developer tools

Three first-party, dependency-free subsystems that ship with Forge:

- `forge::script` — a small deterministic interpreter (no Lua).
- `forge::json` — a header-only JSON value/parser/writer shared by tilemaps,
  saves and prefabs (no exceptions).
- `forge::DebugOverlay` — an F1 overlay (no ImGui).

All three are in the umbrella header.

## forge::json

Header: `forge/utils/json.hpp`. Header-only, no exceptions, no RTTI. Objects
keep **insertion order** (handy for diffs and hand-edited files).

```cpp
#include <forge/forge.hpp>

forge::json::Value doc = forge::json::Value::object();
doc.set("name", forge::json::Value("hero"));
doc.set("hp", forge::json::Value(12));
doc.set("pos", forge::json::Value::array());
doc["pos"].push(forge::json::Value(100.0));
doc["pos"].push(forge::json::Value(200.0));

std::string pretty = forge::json::write(doc, true);   // true = indent
bool ok = forge::json::writeFile("save.json", doc, true); // atomic: tmp + rename
```

Reading:

```cpp
forge::json::ParseError err;
forge::json::Value v = forge::json::parseFile("save.json", &err);
if (!err.ok()) {
    // err.line / err.column / err.message (1-based line, 0 = I/O error)
    FORGE_LOG_ERROR("save.json: %s", err.str().c_str());
}
std::string name = v["name"].asString("unnamed");
int hp           = v["hp"].asInt(1);
float x          = v["pos"].at(0).asFloat(0.0f);
for (const auto& [key, value] : v.items()) { /* ordered */ }
```

Value API at a glance:

```cpp
enum class Type { Null, Bool, Number, String, Array, Object };
static Value object();  static Value array();
Value();  Value(bool);  Value(int);  Value(double);  Value(const char*);
Value(std::string);

Type type() const;  bool isNull/isBool/isNumber/isString/isArray/isObject() const;
bool   asBool(bool def = false) const;
double asDouble(double def = 0.0) const;
int64_t asInt64(int64_t def = 0) const;
int    asInt(int def = 0) const;
float  asFloat(float def = 0.0f) const;
const std::string& asString(const std::string& def = {}) const;

size_t size() const;
const Value& at(size_t i) const;          // arrays
const Value& operator[](const std::string& key) const;  // objects (null if absent)
Value& operator[](const std::string& key); // creates on write
void set(const std::string& key, Value v);
void push(Value v);
const std::vector<std::pair<std::string, Value>>& items() const;
const std::vector<Value>& elements() const;
```

Behavior worth knowing:

- `parse` returns a null `Value` on failure and fills `ParseError`; it never
  throws and never partially mutates your data.
- Strings accept the full escape set including `\uXXXX` and UTF-16 surrogate
  pairs (encoded to UTF-8); lone surrogates become U+FFFD. Control characters
  are rejected.
- Non-finite numbers (`NaN`/`inf`) are written as `0` so the output is always
  valid JSON.
- `writeFile` writes to `<path>.tmp` and renames over the target, so a crash
  mid-save cannot corrupt the previous file.
- Accessors take defaults: `v["missing"].asInt(7) == 7`. Missing keys and
  wrong types both fall back, which keeps save-file migrations short.

## forge::script

Header: `forge/script/script.hpp`. A small interpreter: numbers (double),
booleans, strings, `null`, and ordered tables. No classes, no closures, no
metatables — deliberately.

```fgs
var speed = 120;
var jumps = 0;

fn on_update(dt, self) {
    if (self.left) { self.x = self.x - speed * dt; }
    if (self.jump && self.on_ground) { self.vy = 420; jumps = jumps + 1; }
    while (jumps > 3) { jumps = jumps - 1; }
    return jumps;
}
```

Language: `var`, assignment, `if`/`else`, `while`, `for (init; cond; step)`,
`return`, `fn name(a, b) { ... }`, `&& || !`, `== != < <= > >=`, `+ - * / %`,
unary minus, string concatenation with `+`, table literals `{x = 1, y = 2}`,
member access `self.x`, index access `arr[0]`, function calls, and comments
with `//` and `/* */`.

Host integration:

```cpp
forge::script::ScriptVM vm;

// Values passed in/out
vm.setGlobal("speed", 120.0);
vm.setGlobal("self", forge::script::Value::table());
vm.bind("spawn", [](const std::vector<forge::script::Value>& a) {
    // a[0], a[1], ...
    return forge::script::Value(true);
});
vm.printTo([](const std::string& line) { FORGE_LOG_INFO("[fgs] %s", line.c_str()); });

if (!vm.loadFile("scripts/player.fgs"))          // or loadSource(text, name)
    FORGE_LOG_ERROR("%s", vm.error().str().c_str());

auto r = vm.call("on_update", forge::script::args(dt, selfValue));
double jumps = r.number(0.0);
if (!vm.error().ok()) FORGE_LOG_ERROR("%s", vm.error().str().c_str());
```

API:

```cpp
bool loadSource(const std::string& src, const std::string& name = "<script>");
bool loadFile(const std::string& path);
void clear();

Value call(const std::string& fn, const std::vector<Value>& args = {});
bool hasFunction(const std::string& name) const;
std::vector<std::string> functions() const;   // by value
bool loaded() const;  const std::string& sourceName() const;

void setGlobal(const std::string& name, Value v);
Value getGlobal(const std::string& name) const;
void bind(const std::string& name, HostFn fn);
void printTo(std::function<void(const std::string&)> fn);

void setStepBudget(uint64_t steps);   // default 1,000,000 per call
uint64_t lastStepCount() const;
const Error& error() const;           // .ok() / .line / .column / .message / .str()
void clearError();

// helpers
Value args(...);                      // variadic -> std::vector<Value>
```

Rules that matter:

- **Errors never throw.** Parse errors are reported with line/column and
  `loadSource` returns false. Runtime errors abort the current `call()` and
  leave the VM usable for the next one. Check `vm.error()` after `load`/`call`
  if correctness matters.
- **Determinism**: no hash-order iteration (tables are ordered vectors), no
  clock, no RNG. The step budget (checked in loops and call dispatch) makes an
  accidental infinite loop a reported error instead of a hung game.
- Top-level statements run **once**, lazily, on the first `call()` after a
  load; `clear()`/`loadSource` reset that.
- Tables have shared-reference semantics (`shared_ptr<Table>`), so passing a
  table to a function and mutating it is visible to the caller — that is how
  `self` works. Copying a `Value` copies the reference, not the contents.
- `vm.call()` on a missing function reports an error (does not throw) and
  returns a null value.
- Hot reload is just `loadFile` again: `10_lantern` binds F2 to it and shows
  the script's state in the HUD.

## DebugOverlay

Header: `forge/utils/debug_overlay.hpp`. A first-party replacement for ImGui
for the things a 2D game actually needs in a debug build.

```cpp
forge::DebugOverlay overlay;
overlay.init(&font);                       // one Font is enough

// per frame, after the world + HUD:
overlay.update(dt, forge::Profiler::instance(), batch);
overlay.draw(batch, engine.viewport(), &scene, &scene.physics());
```

It draws, in screen space (centered Y-up ortho):

- an FPS/frame-time graph with a 16.7 ms reference line and a 1% low readout,
- frame/update/render ms, draw calls and quad count,
- entity count and physics body/joint counts,
- the last profiler zone,
- a small text panel.

Options:

```cpp
forge::DebugOverlayOptions o;
o.enabled = true; o.fpsGraph = true; o.stats = true;
o.scene = true; o.physics = true; o.logTail = true;
o.scale = 1.0f;
o.background = forge::Color{0.04f, 0.05f, 0.08f, 0.86f};
overlay.setOptions(o);

overlay.handleInput(engine.input());   // F1 toggles
bool on = overlay.enabled();
```

- `draw` must run between `begin()`/`end()` with a **screen-space** matrix
  (the same centered ortho as `engine.defaultProj()`); it does not begin its
  own pass.
- `update` takes real frame times; feeding a fake `1/60` makes the graph a
  flat line.
- `averageFps()` and `onePercentLowMs()` are exposed for tests and for your
  own HUD.
- It is not a general-purpose immediate-mode UI: no windows, no widgets, no
  input fields. That keeps the engine's zero-dependency promise intact.

## Putting them together

`examples/10_lantern.cpp` is the reference integration: a tilemap world with a
ramp, a player with a lantern light, collectible entities in a `Scene`, save
and load with `serialization.hpp`, score/objective rules in a `.fgs` file that
hot-reloads on F2, a vignette post pass, and the debug overlay on F1. Run it
with `CI=1 ./build/10_lantern --frames 240`, capture a frame with
`./build/10_lantern --shot out.png --frames 300` (add `--overlay` to include
the panel), or exercise the save/load path headlessly with
`./build/10_lantern --selftest`.
