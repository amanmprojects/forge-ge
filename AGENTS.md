# AGENTS.md — Forge (C++20 2D game engine)

Forge is a batteries-included 2D game engine: one static library (`forge`),
one umbrella header (`<forge/forge.hpp>`), SDL3 + OpenGL 3.3 core, everything
else vendored (stb_image, stb_truetype, stb_image_write, miniaudio, a hand-
written GL loader). This file is the fast path; the full documentation lives
in [`docs/`](docs/README.md).

## Build, test, run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build                     # headless unit tests
./build/06_verify                          # render self-test, exit 0 = verified
./build/01_hello                           # shapes + text + camera
CI=1 ./build/07_breakout --frames 240      # smoke run, exit 0/1
CI=1 ./build/09_chess                      # same for chess, exit 0/1
CI=1 ./build/10_lantern --frames 240       # integration example
./build/10_lantern --selftest              # save/load round-trip, exit 0/1

./web/build.sh                              # WebAssembly build -> web/dist (5 games + gallery)
./web/record.sh                             # gallery clips (--record + ffmpeg) -> web/dist/clips
python3 -m http.server 8000 -d web/dist     # then open http://localhost:8000/
./web/deploy.sh                             # web/dist -> gh-pages (GitHub Pages)
```

Debug single-config builds get ASan+UBSan automatically. The library, examples
and tests compile with `-Wall -Wextra -Wpedantic` and must stay warning-free.

## Reading order for a new task

1. [`docs/agents/00-overview.md`](docs/agents/00-overview.md) — architecture, coordinates, frame lifecycle
2. [`docs/agents/01-getting-started.md`](docs/agents/01-getting-started.md) — minimal working game + loop patterns
3. The relevant API page (`02-core` … `08-audio-assets`)
4. [`docs/agents/09-recipes.md`](docs/agents/09-recipes.md) — task patterns (HUD, camera, physics, capture, …)
5. [`docs/agents/10-pitfalls.md`](docs/agents/10-pitfalls.md) — check before debugging
6. `examples/` — the ten examples are the canonical usage reference

## Ground rules

- `#include <forge/forge.hpp>` only; don't include GL headers (use `forge::gl::*` wrappers).
- Y-up world, pixels as units. Mouse is top-left SDL coords — convert with
  `Camera2D::screenToWorld`.
- Pair every `batch.begin()` with `batch.end()` once per frame. Draws are
  painter's order; there is no depth test.
- `SpriteBatch::draw(SpriteDraw)` takes a **center** position and a **UV**
  `src` rect; the region helpers and `Sprite::src` take **pixel** rects.
- One `Engine` per thread; everything is main-thread only.
- `EngineConfig::window` applies at construction (fixed in 0.2.0); use
  `engine.viewport()` for layout and `engine.window().setTitle(...)` to
  retitle at runtime.
- Fixed-step update may run 0..maxSteps times per frame; keep `update`
  deterministic and `render` free of simulation state. Input edges commit
  after each observed step — a press on a 0-step frame is delivered once to
  the next `update`, never dropped and never seen twice.
- Physics is arcade-grade: no rotation, unit masses, one relaxation pass;
  default gravity `(0, -980)`. `Body*` pointers die on `addBody/removeBody`.
- ECS `view<Ts>()` must not create/destroy entities or add/remove components
  inside the callback; defer structural changes until after.
- Check load results: `loadTexture/loadFont/loadSound` can fail (missing file,
  font atlas overflow, no audio device). Audio soft-fails to muted — never
  assume a sound played.
- 0.2.0 adds `LightingSystem`, `PostProcess`/`FullscreenPass`, `AtlasPacker`,
  `Skeleton`, `forge::script`, `forge::json`, `DebugOverlay`, scene
  serialization/prefabs, bitmap fonts, slopes, physics filtering/joints and
  install/export (`find_package(forge)`, `forge::forge`). `docs/ROADMAP.md`
  tracks the remaining plan.
- The example games are also tests: `--shot file.png` writes a deterministic
  PNG, `--record clip.mp4` pipes one offscreen frame per fixed step to ffmpeg
  (`examples/common/clip.hpp`), `CI=1` runs a smoke mode with an exit code.
  Copy `examples/07_breakout.cpp` for that pattern.
- `examples/chess/chess.hpp` (0x88 rules) and `examples/chess/ai.hpp` (negamax
  + eval) are engine-free, header-only and shared with `tests/test_chess.cpp`
  (perft + rules suite). Only `examples/chess/main.cpp` touches Forge, so the
  chess core can change without rebuilding the library and stays headless.
- `examples/chess/assets/` holds the generated piece sprites (`wK.png` …
  `bP.png`, plus the flat `tP.png` … `tK.png` tray set, rasterized from
  `svg/*.svg` by `make_pieces.sh` — the Cburnett set of `amanmprojects/chess`,
  CC BY-SA 3.0), loaded with `AssetManager::loadTexture` from probed paths; one
  sprite is one whole board square, and if the folder is absent the game draws
  procedural silhouettes, so both paths must keep working.
- Web builds (`./web/build.sh`) take the frame loop over with
  `emscripten_set_main_loop_arg`, which unwinds `main()`'s stack (running its
  locals' destructors, since the web build uses `-fwasm-exceptions`): engine
  and game must be statics or globals on `FORGE_PLATFORM_WEB` — not
  `unique_ptr` locals — and the loop lambdas must not capture `main()`'s locals. Never spawn threads
  there — pthreads would need COOP/COEP headers. `Engine::quit()` is a no-op
  on the web. Each game page is `web/shell.html.in` filled in by
  `forge_web_page()` in `CMakeLists.txt`. See [`web/README.md`](web/README.md).

## Doc map

| File | Covers |
|---|---|
| `docs/agents/00-overview.md` | architecture, coordinate conventions, frame lifecycle, threading, module map |
| `docs/agents/01-getting-started.md` | build, minimal game, the three loop styles, project integration |
| `docs/agents/02-core.md` | `Engine`, `Window`, `Time`/`FixedStepper`, `Input`, `Logger`, `Profiler` |
| `docs/agents/03-math.md` | `Vec2/IVec2/Mat4/Rect/Circle/Color`, `Random`, easing, noise, narrowphase |
| `docs/agents/04-graphics.md` | `SpriteBatch`, `Camera2D`, `Texture2D`, `Font`, `Framebuffer`, `Tilemap`, `AtlasPacker`, `LightingSystem`, `PostProcess`, `Skeleton`, `Shader`/GL |
| `docs/agents/05-animation-particles.md` | `Animator`, `Tween`/`Tweens`, `ParticleSystem` |
| `docs/agents/06-physics.md` | `PhysicsWorld` bodies, solver rules, filtering, sensors, one-way, joints, CCD, queries |
| `docs/agents/07-scene-ecs.md` | `Registry`/ECS, components, `Scene` pipeline + physics sync, scene/prefab save/load |
| `docs/agents/08-audio-assets.md` | `AudioEngine`, in-memory WAV synthesis, crossfades, `AssetManager` search paths + hot reload |
| `docs/agents/09-recipes.md` | task-oriented patterns with copyable code |
| `docs/agents/10-pitfalls.md` | every gotcha with symptom → cause → fix |
| `docs/agents/11-scripting-tools.md` | `forge::script` (`.fgs`), `forge::json`, `DebugOverlay` |
| `docs/ROADMAP.md` | the living 0.1.0 → 1.0 plan, acceptance criteria, non-goals |
| `web/README.md` | WebAssembly/WebGL2 build, deploy, and what the port changes |

## Example map

| Example | Shows |
|---|---|
| `examples/01_hello.cpp` | window, camera, shapes, text, `screenToWorld`, `CI=1` auto-quit |
| `examples/02_platformer.cpp` | `PhysicsWorld` platformer, procedural tileset, tilemap solids, particles, camera follow/shake |
| `examples/03_benchmark.cpp` | 10k sprites, draw-call stats, auto-exit |
| `examples/04_audio.cpp` | synthesized WAV → `loadMemory`, volumes/pan |
| `examples/05_scene.cpp` | `Scene`/ECS swarm + crates, scripts, contact-driven gameplay |
| `examples/06_verify.cpp` | render-correctness self-test (offscreen readback, scissor, MSAA, post pass, lighting, UTF-8) |
| `examples/07_breakout.cpp` | full game: grid collision without physics, headless `--shot` capture, autopilot, pause |
| `examples/08_snake.cpp` | full game: tile grid, chiptune SFX synthesis, title/pause/game-over states, golden bonus logic |
| `examples/10_lantern.cpp` | integration: Tiled map + slope, lantern lighting, vignette post pass, `.fgs` hot reload (F2), scene save/load (F5/F9), `DebugOverlay` (F1), touch, `--shot`/`CI=1` |
| `examples/chess/main.cpp` | full game: rules-complete chess (0x88 + Zobrist + SAN), negamax bot with 5 levels, textured piece sprites (`assets/`, procedural fallback), promotion/undo/hint/flip UI, `--fen`/`--moves`/`--click` scripted input |

## Conventions of this repo

- C++20, no exceptions in hot paths, no RTTI use, std library only (`std::`).
- Fixed-size types from `<cstdint>`; floats for gameplay math.
- Public API in `include/forge/<module>/*.hpp`, implementation in
  `src/<module>/*.cpp`, declared sources only (no glob) in `CMakeLists.txt`.
- One feature per example commit; keep `README.md` example list in sync when
  adding an example.
