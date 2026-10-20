# 02 — Core: Engine, Window, Time, Input, Logger, Profiler

Headers: `forge/core/engine.hpp`, `window.hpp`, `time.hpp`, `input.hpp`,
`logger.hpp`, `utils/profiler.hpp`.

## Engine

`forge::Engine` owns the window, GL context, input state, time, sprite batch,
audio engine and asset manager, and drives the frame loop.

```cpp
struct EngineConfig {
    WindowConfig window;          // applied when the Engine is constructed
    float fixedStep = 1.0f / 60.0f;  // runFixed update step
    int   maxSteps  = 5;             // max update calls per rendered frame
    bool  initAudio = true;          // soft-fails to muted
    bool  quitOnEscape = false;      // Engine quits on ESC keydown
    int   maxFps = 0;                // sleep-based cap; 0 = uncapped
};
```

> **Fixed in 0.2.0:** `Engine` now constructs its `Window` from
> `EngineConfig::window`, so title/size/vsync/MSAA/fullscreen apply at
> creation. `engine.window().config()` reports what was applied;
> `setTitle(...)` still works at runtime.

Construction: `explicit Engine(const EngineConfig& cfg = {});` — non-copyable.
Check `engine.valid()`; on failure the engine logs `FORGE_LOG_FATAL` and all
systems are absent.

### Subsystem accessors

```cpp
Window&       window();    Input&       input();    Time&    time();
SpriteBatch&  batch();     AudioEngine& audio();    AssetManager& assets();
```

### Loops and manual frame control

```cpp
void runFixed(std::function<void(float)> update, std::function<void()> render);
void runVariable(std::function<void(float)> frame);

bool beginFrame(const Mat4& viewProj);  // false = quit; then do NOT call endFrame
void endFrame();                        // flush batch + swap + profiler end
void clear(float r, float g, float b, float a = 1.0f);  // full-viewport glClear

void quit();  bool quitting() const;
IVec2 viewport() const;   // drawable pixels (HiDPI aware)
Mat4  defaultProj() const;// ortho centered on viewport, pixels as units
float fps() const;  int drawCalls() const;
```

Frame-loop behavior worth knowing:

| | `runVariable` | `runFixed` | manual `beginFrame` |
|---|---|---|---|
| pumps events | yes | yes | yes |
| input commit (edges consumed) | after `frame(dt)` | after each `update` step | at `beginFrame` |
| advances `Time` | yes | yes | yes (`time().delta()`) |
| calls your code | 1× per frame with clamped dt | `update` 0..maxSteps×, then `render` 1× | you decide |
| buffer swap | engine | engine | `endFrame` |
| `maxFps` pacing | yes | yes | **no** |
| `assets().update()` hot-reload poll | yes | yes | **no** (call it yourself) |
| interp alpha given | — | no | — |

`beginFrame(viewProj)` does: profiler begin → `input().beginFrame()` (commit the
previous input window) → event pump (returns false on quit) → `time().tick()` →
`batch().resetStats()` → full-viewport `glViewport` → `batch().begin(viewProj)`.
So inside a manual loop the batch is already begun; draw, then `endFrame()`.

Input edges are delivered to the code that observes them, exactly once: in
`runFixed` the commit happens after every `update` step, so a key pressed
during a frame that ran **zero** steps still reports `pressed()` on the next
step (at 144 Hz vsync against a 60 Hz step most frames are 0-step). In
`runVariable` the commit is after `frame(dt)`. In a manual loop
`Engine::beginFrame()` commits at the start of the frame. See the Input
section below.

`clear()` targets the currently bound framebuffer (with the window viewport).
If a `Framebuffer` is bound you'll clear the FBO instead — see 04.

### Stats

- `fps()` — smoothed frames/second from `Time`.
- `drawCalls()` — flush count of the current frame (reset by the loops in
  `beginFrame`/`runFixed`/`runVariable`).

## Window

`EngineConfig::window` is honoured as of 0.2.0: the `Engine` constructs its
`Window` from the config, so title/size/vsync/fullscreen/MSAA apply at
creation. `engine.window().config()` returns the applied `WindowConfig` and
`setTitle()` updates it.

Owned by `Engine`; rarely used directly.

```cpp
struct WindowConfig {
    int width = 1280, height = 720;
    std::string title = "Forge Game";
    bool resizable = true, vsync = true, fullscreen = false, highDPI = true;
    int msaaSamples = 0;                 // 0, 4, 8
};

bool  valid() const;
void  swapBuffers();
void  setTitle(const std::string& t);
const std::string& title() const;
const WindowConfig& config() const;   // the config the window was created with
void  setVSync(bool on);   bool vsync() const;
IVec2 size() const;         // logical window size
IVec2 drawableSize() const; // physical pixels (use this for rendering)
float aspect() const;       // w/h
float dpiScale() const;     // drawable / logical
void* nativeHandle() const; void* glContext() const; // SDL types, if you really need them
```

On resize the engine updates the window size and calls `glViewport(0,0,dw,dh)`.
The ortho you build from `engine.viewport()` therefore adapts automatically —
but layouts that hardcode pixel offsets do not. `Window::onResize` clamps zero
sizes (minimized windows) to 1 to keep projections valid.

## Time and FixedStepper

```cpp
class Time {
    float tick();                 // call once per frame; returns clamped delta
    float delta() const;          // clamped frame delta (max 0.1 s by default)
    float rawDelta() const;       // unclamped
    double elapsed() const;       // seconds since construction
    uint64_t frame() const;       // frame counter
    float fps() const;            // smoothed
    void setMaxDelta(float d);    // clamp for hitches
};
```

`FixedStepper` is public and reusable if you want a fixed step inside your own
update:

```cpp
class FixedStepper {
    FixedStepper(float step = 1.0f/60.0f, int maxSteps = 5);
    template<typename Fn> void update(float dt, Fn&& fn); // calls fn(step) 0..maxSteps times
    float alpha() const;   // accumulator/step — interpolation factor 0..1
};
```

If the accumulator is still ≥ step after `maxSteps` calls it is zeroed (the
backlog is dropped on purpose; no death spiral). `alpha()` is not consumed by
`runFixed` — it's there if you run the stepper yourself and want to interpolate
rendering.

## Input

Filled by the SDL event pump once per rendered frame; committed ("end of
input window") after the code that consumes it actually runs — after each
`update` step in `runFixed`, after `frame(dt)` in `runVariable`, at
`beginFrame` in a manual loop. Queries are cheap and can be called repeatedly.

Sources: keyboard, mouse, up to 4 gamepads, and touch (SDL finger events,
normalized window coordinates converted to drawable pixels). IME composition
events feed the text-editing state.

### Keyboard

Keys are a fixed enum (`forge::Key`): `Space`, `A`…`Z`, `N0`…`N9`, `Up/Down/
Left/Right`, `Escape`, `Enter`, `Tab`, `Backspace`, shifts/ctrls/alts, `F1`–
`F12`, and punctuation (`Minus`, `Equals`, `LBracket`, `RBracket`,
`Semicolon`, `Quote`, `Comma`, `Period`, `Slash`, `Backslash`). Note the
letter order in the enum is not alphabetical — compare by name, never by
integer. `Key::Unknown` is returned for unmapped scancodes; `Key::Count`
terminates the enum.

```cpp
bool isDown(Key) const;      // held
bool pressed(Key) const;     // went down this input window (incl. sub-step tap)
bool released(Key) const;    // went up this input window
static Key keyFromName(std::string_view);  // "space","escape","enter","tab","up"…,"a".."z","0".."9"
```

`pressed()`/`released()` also report taps that went down *and* up between two
commits (a latch is set on the transition and cleared when the window
commits), so single-step taps can't be missed by slow update code. Every edge
is observed exactly once: it survives as many 0-step frames as it takes until
an `update` runs, and a frame that runs several steps reports it in the first
step only.

### Mouse

```cpp
bool mouseDown/MousePressed/MouseReleased(MouseButton) const; // Left,Right,Middle,X1,X2
Vec2 mousePos() const;     // drawable pixels, top-left origin
Vec2 mouseDelta() const;   // this window - previous window
Vec2 wheel() const;        // accumulated scroll this window ({0,0} normally)
```

`mousePos()` is converted to drawable pixels for you. World position:
`cam.screenToWorld(in.mousePos(), vp.x, vp.y)`.

### Gamepads (4 slots)

```cpp
enum class GamepadButton : uint8_t {
    A, B, X, Y, Back, Start, Guide, LeftStick, RightStick,
    LeftShoulder, RightShoulder, DpadUp, DpadDown, DpadLeft, DpadRight, Count
};  // Forge-local layout: A = south/pad-bottom, B = east (Xbox naming)

bool  padDown/padPressed/padReleased(int pad, GamepadButton) const;
float padAxis(int pad, int axis) const; // 0..5 = LX,LY,RX,RY,LT,RT; sticks -1..1 (Y down), triggers 0..1
bool  anyPadDown(GamepadButton) const;
int   padCount() const;
```

Stick axes get a 0.12 deadzone applied when events come in. `Engine` opens up
to 4 gamepads at startup and hotplug connects/disconnects them.

### Action mapping

Bind logical actions once, query them from any device:

```cpp
in.bindKey("jump", forge::Key::Space);
in.bindKey("jump", forge::Key::W);
in.bindPad("jump", forge::GamepadButton::A);
in.bindMouse("fire", forge::MouseButton::Left);

if (in.actionPressed("jump")) { /* ... */ }
bool held    = in.actionDown("jump");
bool letGo   = in.actionReleased("jump");
in.clearBinds();
```

`actionDown/Pressed/Released` return true if **any** bound source matches.
Pad bindings match any connected pad in `actionPressed/Released`, but
`actionDown` uses `anyPadDown`, which also works if a pad connects later.

### Movement axis helper

```cpp
Vec2 moveAxis(int pad = -1) const;
void setStickDeadzone(float dz);   // clamped to 0..1, default 0.18
float stickDeadzone() const;
void setStickCurve(float power);   // response curve exponent, default 1.0
float stickCurve() const;
```

Combines WASD + arrows + one pad's sticks/dpad (or **all connected pads** when
`pad == -1`) into a Y-up vector, sticks applied with `y -= stickY`, length
clamped to 1. Use `moveAxis(0)` when you want a specific pad only.

The deadzone is radial (the whole stick vector is remapped so output starts at
0 outside the deadzone and still reaches 1 at the rim), then `stickCurve`
applies `pow(magnitude, curve)` — `1.0` is linear, `> 1` gives finer control
near the center.

### Text input and IME

```cpp
void pushText(const char* utf8);      // fed from SDL_EVENT_TEXT_INPUT by Engine
std::string consumeText();            // takes and clears this frame's text

// IME composition preview (CJK input, etc.). `cursor` is a UTF-8 byte offset
// into the composition string, or -1 when the IME has no selection.
void setComposition(std::string utf8, int cursor = -1);
const std::string& composition() const;
int compositionCursor() const;
bool composing() const;
```

Useful for high-score name entry and any text field. Text accumulates during
the current input window and is cleared when it commits (after `frame(dt)` /
after each `update` step / at the next `beginFrame` in a manual loop) — the
code that consumes it gets it exactly once. The composition string is cleared
when the window commits; the IME sends a new one on every keystroke.

Draw the composition underlined or with a caret at `compositionCursor()`; do
not insert it into your buffer until `SDL_EVENT_TEXT_INPUT` delivers the
committed text.

### Touch

```cpp
struct TouchPoint {
    int64_t id;
    Vec2 pos;        // drawable pixels, top-left origin (same as mousePos())
    Vec2 start;      // where this finger went down
    float pressure;  // 0..1 (device dependent)
};

bool touchDown(int64_t id, Vec2 pos, float pressure = 1.0f);
void touchMove(int64_t id, Vec2 pos, float pressure = 1.0f);
void touchUp(int64_t id);
void touchCancel(int64_t id);

const std::vector<TouchPoint>& touches() const;
const TouchPoint* touch(int64_t id) const;   // nullptr when not down
Vec2 touchDrag(int64_t id) const;            // pos - start (zero when lifted)
bool touchActive() const;                    // any finger down
bool touchPressed() const;                   // a finger went down this window
bool touchReleased() const;                  // a finger lifted this window
```

`Engine` feeds these from `SDL_EVENT_FINGER_DOWN/MOTION/UP/CANCELED`. The
press/release latches are window-scoped exactly like key latches: a quick tap
is never dropped. Convert to world space with `cam.screenToWorld(p, vp.x,
vp.y)`, same as the mouse. Multi-touch is supported (ids come from SDL).

### Persisted action remaps

```cpp
std::string serializeBinds() const;             // "jump=space,z,mouse:left,pad:a\n..."
bool deserializeBinds(const std::string& text, std::string* error = nullptr);
bool saveBinds(const std::string& path) const;  // atomic-ish text file
bool loadBinds(const std::string& path, std::string* error = nullptr);
```

One action per line, `action=source,source,...`, `#` starts a comment. Sources
are key names (`keyFromName` names), `mouse:<left|right|middle|x1|x2>` and
`pad:<a|b|x|y|back|guide|start|ls|rs|lb|rb|up|down|left|right>`. Parsing is
strict: an unknown token fails with `line N: ...` in `error` and leaves the
previous bindings untouched. Bind files are plain text, so shipping a default
`binds.txt` next to the game and letting the player edit it works without any
extra format.

## Logger

`forge::Logger` — a singleton, level-filtered, colored stderr logger with an
optional file sink.

```cpp
FORGE_LOG_TRACE/DEBUG/INFO/WARN/ERROR/FATAL("fmt %d", x);
forge::Logger::instance().setLevel(forge::LogLevel::Debug);   // default Info
forge::Logger::instance().setFile("game.log");                // empty = off
```

The macros check `enabled(level)` *before* formatting (hot-path friendly) and
include `__FILE__`/`__LINE__`. `FORGE_LOG_FATAL` is always emitted. Use `%s`
with `std::string::c_str()` — the format string goes straight to `snprintf`
(2048-byte buffer).

## Profiler

Compiled behind `FORGE_PROFILING` (CMake option `FORGE_ENABLE_PROFILING`,
always defined to 0/1). When 0, every entry point is an inline no-op.

```cpp
FORGE_ZONE("physics");        // scoped CPU zone; also FORGE_ZONE_FN()
forge::Profiler::instance().frameMs();     // last frame ms
forge::Profiler::instance().zoneMs("physics");
forge::Profiler::instance().zones();       // vector<ZoneRecord>
forge::Profiler::instance().history();     // up to 120 recent frame times, seconds→ms floats
```

The loops call `beginFrame`/`endFrame` for you; `runFixed` even wraps your
update and render in `FORGE_ZONE("update")` / `FORGE_ZONE("render")`. Nested
zones are supported (a stack). `engine` also has `fps()` for a cheap on-screen
counter.

```cpp
// Minimal overlay:
char buf[64];
std::snprintf(buf, sizeof(buf), "%.2f ms  (%d draws)",
              forge::Profiler::instance().frameMs(), engine.drawCalls());
font.draw(batch, buf, {-vp.x/2 + 8, vp.y/2 - 8}, 0.75f, forge::Color::white());
```
