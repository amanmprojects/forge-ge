# 01 — Getting started

## Requirements

- C++20 compiler (GCC 13+, Clang 16+), CMake 3.20+
- SDL3 development files (`pkg-config sdl3`), OpenGL headers/drivers

```bash
# Arch
sudo pacman -S sdl3 cmake ninja
# Ubuntu 24.04+ (SDL3 may need backports or a source build)
sudo apt install cmake ninja-build
```

## Build the repo

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build          # headless unit tests
./build/01_hello                # first example
```

CMake options:

| Option | Default | Effect |
|---|---|---|
| `FORGE_BUILD_EXAMPLES` | ON | builds `examples/01..08` |
| `FORGE_BUILD_TESTS` | ON | builds `tests/test_engine` + `ctest` entry |
| `FORGE_ENABLE_PROFILING` | ON | drives `FORGE_PROFILING=0/1`; `0` compiles all zones to no-ops |
| `FORGE_ENABLE_SANITIZERS` | ON for single-config Debug | ASan+UBSan compile *and* link flags |

## Minimal game

The smallest useful Forge program — a window, a camera, a shape, text, input,
and a clean exit:

```cpp
#include <forge/forge.hpp>
#include <cstdio>

int main() {
    forge::EngineConfig cfg;
    cfg.window.title = "My Game";        // NOTE: ignored in 0.1.0 — see below
    forge::Engine engine(cfg);
    if (!engine.valid()) return 1;       // window or GL creation failed

    forge::Camera2D cam;
    forge::Vec2 player{0, 0};

    engine.runVariable([&](float dt) {   // dt = clamped frame seconds
        auto& in = engine.input();
        if (in.isDown(forge::Key::Escape)) engine.quit();

        player += in.moveAxis() * 400.0f * dt;   // WASD/arrows/stick, normalized
        cam.follow(player, 6.0f, dt);
        cam.update(dt);

        forge::IVec2 vp = engine.viewport();
        engine.clear(0.09f, 0.09f, 0.12f);       // clears the bound framebuffer

        auto& batch = engine.batch();
        batch.begin(cam.viewProj(vp.x, vp.y));
        batch.drawCircle(player, 24.0f, forge::Color::red(), 24);
        batch.end();
    });
    return 0;
}
```

Notes:

- `EngineConfig::window` (title/size/vsync/…) is **ignored in 0.1.0** because
  `Engine` default-constructs its `Window` member before the config applies.
  Use `engine.window().setTitle("…")` at runtime. The default window is
  1280×720, resizable, vsync on.
- `runVariable` calls your callback exactly once per frame with the clamped
  frame delta (`Time::maxDelta`, default 0.1 s).
- `engine.clear(r,g,b)` is the intended way to begin a frame; it sets the
  full-window viewport and clears whatever framebuffer is bound.

## The three loop styles

### 1. Variable step — `runVariable(frame)`

Use for menus, UI, anything frame-rate independent or not worth a fixed step.
`frame(dt)` is called once per frame; you own rendering. Input is committed
right after your callback returns, so edges pumped this frame are visible to
it (exactly once).

```cpp
engine.runVariable([&](float dt) {
    updateVisuals(dt);
    render(engine.batch());          // begin/end inside
});
```

### 2. Fixed step — `runFixed(update, render)`

Physics and gameplay that must be deterministic: the engine accumulates real
time and calls `update(1/60)` **0..maxSteps times** (default 5) per frame,
then `render()` **exactly once**. After a long hitch the backlog is dropped
(no spiral of death). `render()` does not receive an interpolation alpha —
design render to read the latest simulation state, or smooth in the objects
themselves.

Read input in `update`: each update step commits the input window when it
returns. A key pressed during a frame that ran **0 steps** keeps its
`pressed()` edge until the next step that runs, and a frame that runs several
steps reports the edge to the first one only — so games never need
"press it again" retries and never double-register a press.

```cpp
forge::PhysicsWorld world;
world.setGravity({0, -2200});

engine.runFixed(
    [&](float dt) {              // may run several times per frame
        world.step(dt);
        // gameplay: input, AI, spawn/despawn
    },
    [&]() {                      // once per frame, after all updates
        forge::IVec2 vp = engine.viewport();
        engine.clear(0.07f, 0.07f, 0.1f);
        auto& batch = engine.batch();
        batch.begin(cam.viewProj(vp.x, vp.y));
        // draw world
        batch.end();
    });
```

### 3. Manual stepping — `beginFrame` / `endFrame`

For tests, headless capture, or custom loops. You do everything yourself:

```cpp
const float FIXED_DT = 1.0f / 60.0f;
for (int frame = 0; frame < 600; ++frame) {
    if (!engine.beginFrame(cam.viewProj(engine.viewport().x, engine.viewport().y)))
        break;                             // window closed: DO NOT call endFrame
    engine.clear(0.1f, 0.1f, 0.12f);
    simulate(FIXED_DT);                    // your fixed dt, not wall clock
    draw(engine.batch());                  // batch is already begun
    engine.endFrame();                     // flush + swap
}
```

`beginFrame` pumps events, commits the previous input window, advances `Time`,
and calls `batch.begin(viewProj)`.
On `false` (quit event) nothing was begun — calling `endFrame` would flush and
swap an empty frame and unbalance the profiler.

Manual loops do **not** call `AssetManager::update()` (hot reload) and do
**not** apply `EngineConfig::maxFps` pacing; call `engine.assets().update()`
yourself if you want hot reload.

## Using Forge from another project

Option A — `add_subdirectory`:

```cmake
add_subdirectory(third_party/forge)     # wherever you vendored it
target_link_libraries(mygame PRIVATE forge)
```

Option B — link the built library directly:

```bash
g++ -std=c++20 game.cpp -o game \
    -I path/to/forge/include -I path/to/forge/third_party \
    $(pkg-config --cflags sdl3) -L path/to/forge/build -lforge \
    $(pkg-config --libs sdl3) -lGL -lm -lpthread -ldl
```

The `third_party` include dir is `PUBLIC` on the `forge` target, so
`#include "stb_image_write.h"` works in games too (the examples use it for
PNG capture).

## First-frame checklist

Everything you'll forget at least once:

1. `if (!engine.valid()) return 1;` after constructing the `Engine`.
2. Check `Texture2D*` / `Font*` / `SoundHandle` results — `nullptr`/invalid
   means the load failed; the game must keep running (or bail) deliberately.
3. One `batch.begin()` … `batch.end()` pair per pass. HUD text usually needs a
   second pair with `engine.defaultProj()`.
4. Use `engine.viewport()` (drawable pixels), not the window size, for
   projections and HUD layout.
5. Only call `engine.endFrame()` if `engine.beginFrame(...)` returned true.
6. Fixed-step `update` can run zero times in a frame — don't put rendering
   state (like "first frame") in it.

## Running the examples as tests

```bash
ctest --test-dir build                 # unit tests (math/ecs/physics/tilemap/anim)
./build/03_benchmark 10000             # stress: prints avg ms, auto-exits
./build/06_verify                      # render self-test, exit 0 = pixels OK
CI=1 ./build/07_breakout --frames 240  # game smoke test, exit 0/1
./build/08_snake --shot /tmp/s.png --frames 700   # deterministic capture
```

Both games parse `--help` for their full flag lists; between them they cover
headless capture, autopilot demos, seeded determinism and CI smoke modes — the
best templates when you need to make your own game testable.
