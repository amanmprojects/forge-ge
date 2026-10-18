<img src="assets/icon.svg" width="64" height="64" alt="Forge logo">

# Forge — a batteries-included 2D game engine in C++

Forge is a high-performance, dependency-light C++20 engine for 2D games.
One static library, one umbrella header, eight runnable examples.

**System dependencies: SDL3 + OpenGL only.** Everything else
(`stb_image`, `stb_truetype`, `miniaudio`, the GL loader) is vendored in
`third_party/` or written from scratch — no GLEW/GLAD, no FreeType, no
header-only JSON frameworks.

Measured on Mesa/NVIDIA (144 Hz): **10,000 rotating sprites at 144 fps in
2 draw calls** (`03_benchmark`, `avgDraw ≈ 1 ms`). Batches flush per 8192
quads or 16 distinct textures, so multi-texture scenes use a few more calls —
still one per flush, never one per sprite.

## Batteries included

| Module | What you get |
|---|---|
| `core` | `Engine` (fixed/variable loop), `Window` (SDL3 + GL 3.3), `Input` (keys, mouse, 4 gamepads, action mapping), `Time`, `Logger` |
| `math` | Header-only `Vec2/Mat4/Rect/Circle/Color`, PCG `Random`, easings, value noise/fBm, narrowphase helpers |
| `graphics` | `SpriteBatch` (8192 quads/flush, 16 texture slots), `Camera2D` (follow, trauma shake), `Texture2D`, `Font` (TTF atlas), `Framebuffer`, `ParticleSystem`, `Animator` + `Tweens`, `Tilemap` (CSV + Tiled JSON, culling, solid queries) |
| `physics` | AABB `PhysicsWorld` (spatial-hash broadphase, substeps, friction/restitution, sensors, raycast, rect queries) |
| `audio` | `AudioEngine` (WAV/MP3/OGG/FLAC via miniaudio, 32-voice pool, music/sfx buses, pan/pitch/loop) |
| `scene` | Generational `ECS Registry` (sparse-set pools, `view<Ts>`), `Scene` with `Transform/Sprite/Velocity/Collider/Script` — collider entities sync into `Scene::physics()` automatically (Velocity present = dynamic, else static) |
| `assets` | `AssetManager` (texture/font/sound cache + mtime hot-reload) |
| `utils` | `Profiler` (scoped zones, frame graph, compiles to no-ops with `FORGE_PROFILING=0`) |

## Documentation

Agent-oriented docs live in [`docs/`](docs/README.md). Start with
[`docs/agents/00-overview.md`](docs/agents/00-overview.md) (architecture and
conventions), use [`docs/agents/09-recipes.md`](docs/agents/09-recipes.md) for
copyable task patterns, and check
[`docs/agents/10-pitfalls.md`](docs/agents/10-pitfalls.md) before debugging.
[`AGENTS.md`](AGENTS.md) is the condensed fast path for coding agents.

## Requirements

- C++20 compiler (GCC 13+, Clang 16+), CMake 3.20+, Ninja/Make
- SDL3 development files (`pkg-config sdl3`), OpenGL headers/drivers

```bash
# Arch
sudo pacman -S sdl3 cmake ninja
# Ubuntu 24.04+ (SDL3 via backports or source)
sudo apt install cmake ninja-build  # then build SDL3 from source
```

## Build & run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build          # headless unit tests (math/ecs/physics/tilemap/anim)
./build/03_benchmark 10000      # 10k-sprite stress test, auto-exits, prints stats
./build/01_hello                # shapes + text + camera (WASD, SPACE shakes)
./build/02_platformer           # physics platformer (arrows/space, R resets)
./build/04_audio                # synthesized SFX (1/2/3, M for music)
./build/05_scene                # 60-entity ECS swarm + falling crates (SPACE pulse)
./build/06_verify               # render-correctness self-test (exit 0 = colors/positions/batch within tolerance)
./build/07_breakout             # Atari Breakout (A/D · arrows · mouse · gamepad, SPACE launch, ESC pause)
./build/08_snake                # classic Snake (WASD/arrows/gamepad, ESC pause, golden apples, bot demo)
```

CMake options: `-DFORGE_BUILD_EXAMPLES=OFF`, `-DFORGE_BUILD_TESTS=OFF`,
`-DFORGE_ENABLE_PROFILING=OFF` (the CMake *option*;
the code macro it drives is `FORGE_PROFILING`, always defined 0/1),
`-DFORGE_ENABLE_SANITIZERS=ON|OFF` (ASan+UBSan; default ON for single-config
Debug builds).

### Headless capture (`07_breakout`, `08_snake`)

Both games double as render/sim tests: with `--shot` they run N fixed 1/60 s
steps under a scripted autopilot, render the final frame into an offscreen
`Framebuffer` and write a PNG. Everything is seeded (`--seed`) and clock-free,
so the same arguments reproduce the same pixels — usable as a screenshot test.

```bash
./build/07_breakout --shot play.png --frames 900                 # mid-game
./build/07_breakout --shot title.png --title                     # title screen
./build/07_breakout --shot clear.png --frames 4100               # level-cleared banner
./build/07_breakout --shot paused.png --frames 700 --pause-at 400 # pause overlay
./build/07_breakout --demo                                       # just watch the bot play
CI=1 ./build/07_breakout --frames 240                            # smoke run, exit 0/1

./build/08_snake --shot play.png --frames 700                    # mid-game
./build/08_snake --shot title.png --title                        # title + bot demo
./build/08_snake --shot over.png --frames 400 --no-ai            # death -> game over
./build/08_snake --shot paused.png --frames 500 --pause-at 450   # pause overlay
./build/08_snake --demo                                          # watch the autopilot play
CI=1 ./build/08_snake --frames 240                               # smoke run, exit 0/1
```

Text is drawn with a TTF picked from the usual system paths (DejaVu Sans Mono
Bold, Adwaita Mono, Liberation Mono, Noto, Menlo, Consolas). If none is found
the HUD and menus fall back to shape-only and the game stays playable — the
examples ship no asset files of their own; sprites, HUD and SFX are all
procedural.

`--shot` also prints `sane=0|1` from an invariant check (Breakout: no NaN,
ball/paddle inside the field, speed capped; Snake: no NaN timers, body/food
inside the grid, step time in range) and exits non-zero if it trips or the
PNG fails to write — so a CI job can assert the games still simulate and
render instead of eyeballing screenshots. Run `--help` on either example for
the full flag list (`--size`, `--no-ai`, `--sloppy`, …).

## Threading

All Forge APIs are **main-thread only** unless noted: GL objects, the
mixer-facing `AudioEngine` setters (lock-free atomics under the hood),
and `Engine::run*` must live on the thread that created the `Engine`.
`AssetManager::update()` is throttled to ~2 Hz filesystem checks.

## Quickstart

```cpp
#include <forge/forge.hpp>

int main() {
    forge::EngineConfig cfg;
    cfg.window.title = "My Game";
    cfg.window.width = 1280;
    cfg.window.height = 720;
    forge::Engine engine(cfg);
    if (!engine.valid()) return 1;

    forge::Camera2D cam;
    forge::Texture2D* hero = engine.assets().loadTexture("hero.png");

    engine.runVariable([&](float dt) {
        auto& in = engine.input();
        if (in.isDown(forge::Key::Escape)) engine.quit();

        cam.follow({0, 0}, 5.0f, dt);
        cam.update(dt);

        forge::IVec2 vp = engine.viewport();
        forge::gl::glViewport(0, 0, vp.x, vp.y);
        forge::gl::glClearColor(0.07f, 0.07f, 0.1f, 1.0f);
        forge::gl::glClear(GL_COLOR_BUFFER_BIT);

        auto& batch = engine.batch();
        batch.begin(cam.viewProj(vp.x, vp.y));
        if (hero) batch.drawTexture(*hero, {0, 0}, hero->size());
        batch.drawCircle({100, 50}, 24, forge::Color::red(), 24);
        batch.end();
    });
}
```

```bash
g++ -std=c++20 game.cpp -o game -I path/to/forge/include -I path/to/forge/third_party \
    $(pkg-config --cflags sdl3) -L path/to/forge/build -lforge \
    $(pkg-config --libs sdl3) -lGL -lm -lpthread -ldl
```

### Fixed-timestep physics loop

```cpp
forge::PhysicsWorld world;
world.setGravity({0, -2200});
// ... add bodies ...
// Arcade solver limits: unit masses, no rotation, single relaxation pass,
// discrete collision (dt clamped to 0.1 s + auto-substeps). Great for
// platformers; not a stacking/rigid-body engine.
engine.runFixed(
    [&](float dt) { world.step(dt); /* gameplay */ },
    [&]           { /* render with engine.batch() */ });
```

### ECS scene

```cpp
forge::Scene scene;
forge::Entity e = scene.create("hero");
scene.registry().get<forge::Transform>(e).pos = {100, 200};
scene.registry().emplace<forge::Velocity>(e, forge::Velocity{{60, 0}});
// per frame: scene.update(dt); scene.draw(engine.batch());
```

## Design notes (why it's fast)
- **One VBO, one shader, minimal state changes.** `SpriteBatch` packs up to
  8192 quads per flush across 16 texture slots; shapes, text and particles all
  ride the same batch — the benchmark draws 10k sprites in 2 draw calls.
- **No per-frame allocations** in hot paths (particles swap-remove, physics
  reuses vectors, ECS pools are flat arrays).
- **Cache-friendly ECS**: components live in contiguous sparse-set pools;
  `view<Ts>()` iterates the pool of the first listed component and filters by
  the rest (list the most selective component first).
- **Spatial-hash broadphase** + fixed-substep solver keeps physics
  deterministic and tunnel-resistant at 60 Hz.
- **Header-only math**, `constexpr` where possible, no virtual calls or RTTI
  in the frame loop.
- **Y-up world, pixels as units.** Textures load flipped so UV `(0,0)` is the
  image bottom-left; `Camera2D::screenToWorld` converts SDL's top-left mouse
  into world space.

## Project layout

```
include/forge/   public headers (forge.hpp umbrella)
src/             implementations (core, graphics, physics, audio, scene, assets, utils)
third_party/     stb_image, stb_truetype, stb_image_write, miniaudio (vendored)
examples/        01_hello … 08_snake
tests/           headless unit tests (ctest)
```

## Roadmap

- TMX infinite maps, autotiling helpers, parallax layers
- 2D lights/shadows, skeletal (Spine-compatible) animation
- Lua scripting bindings, ImGui debug overlay, save/archive format
- Emscripten (WebGL2) + Windows/macOS CI

Contributions welcome — MIT licensed.
