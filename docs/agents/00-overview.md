# 00 — Overview

## What Forge is

A 2D game engine in C++20 built on SDL3 (window/input/audio device) and
OpenGL 3.3 core (rendering). One static library, one umbrella header:

```cpp
#include <forge/forge.hpp>   // everything; this is the only include you need
```

Dependencies are deliberately minimal: SDL3 + OpenGL are the only system
requirements; `stb_image`, `stb_truetype`, `stb_image_write`, `miniaudio` and
a 60-function GL loader are vendored in `third_party/`. No GLEW/GLAD, no
FreeType, no JSON library (the Tiled loader has a small internal parser).

## Module map

| Module | Header dir | What lives there |
|---|---|---|
| core | `include/forge/core/` | `Engine`, `Window`, `Input`, `Time`, `FixedStepper`, `Logger` |
| math | `include/forge/math/` | `Vec2/IVec2/Vec4/Mat4/Rect/Circle/Color`, `Random`, easings, value noise, narrowphase |
| graphics | `include/forge/graphics/` | `SpriteBatch`, `Camera2D`, `Texture2D`, `Font`, `Framebuffer`, `ParticleSystem`, `Animator`/`Tweens`, `Tilemap`, `Shader`, `forge::gl` loader |
| physics | `include/forge/physics/` | `PhysicsWorld` (AABB, arcade solver) |
| scene | `include/forge/scene/` | generational ECS `Registry`, built-in components, `Scene` |
| audio | `include/forge/audio/` | `AudioEngine` (miniaudio mixer) |
| assets | `include/forge/assets/` | `AssetManager` cache + hot reload |
| utils | `include/forge/utils/` | `Profiler` + `FORGE_ZONE` macros |

The headers are the source of truth for signatures. These docs explain the
*behavior* you can't see from a signature.

## The mental model

```
        your game code
              │
   ┌──────────┴───────────┐
   │        Engine        │   owns Window, Input, Time, SpriteBatch,
   │  runVariable/runFixed│   AudioEngine, AssetManager, fixed stepper
   └──────────┬───────────┘
              │ one frame: events → time.tick → update(0..maxSteps) → render → swap
              │            (input commits after each observed update step)
              ▼
   SpriteBatch ──► one shader, one VBO, flushed per 8192 quads / 16 textures
```

- **You own the game**: the engine provides systems (renderer, physics, ECS,
  audio), not a game framework. There is no mandatory base class, no scene
  file format, no asset pipeline.
- **The engine is mostly a set of independent classes.** `PhysicsWorld`,
  `Scene`, `Tilemap`, `ParticleSystem` etc. can be used à la carte. `Engine`
  is only needed for the window/GL/input/audio plumbing.
- **Everything hot is allocation-free**: the sprite batch writes into a
  preallocated staging buffer, particles swap-remove in a fixed pool, physics
  reuses its broadphase storage, ECS pools are flat arrays.

## Coordinates and units

- **Y-up**: world y grows upward. Angles are radians, CCW positive.
- **Pixels are the unit**: a 32×32 sprite is 32 world units. There is no
  pixels-per-meter abstraction.
- **Camera space is centered**: with the default `Camera2D`, world `(0,0)` is
  the center of the window; `Mat4::ortho` maps `-vp/2 .. +vp/2` (see
  `Camera2D::viewProj`). `Engine::defaultProj()` gives the same ortho without
  a camera, for screen-space UI.
- **Textures are stored flipped**: `Texture2D::loadFile` defaults to
  `flipY = true`, so UV `(0,0)` is the *bottom-left of the image* and all the
  "pixel rect" APIs (`drawTextureRegion`, `Sprite::src`, `Tilemap`) treat their
  rects as top-left-origin image rects and convert. If you upload raw pixels
  with `loadMemory`, *you* supply rows bottom-up (see 10-pitfalls).
- **Mouse is top-left SDL coords**, scaled to drawable pixels. Convert with
  `Camera2D::screenToWorld`.
- **Viewport size** is `engine.viewport()` — the drawable (physical) pixel
  size, not the window logical size; they differ on HiDPI. Use it for
  `Camera2D::viewProj(vp.x, vp.y)` and HUD layout.

## Frame lifecycle

Two ways to own the frame:

**A. Callback loops** — `Engine::runFixed(update, render)` /
`Engine::runVariable(frame)`. The engine pumps events, ticks time, calls your
code, swaps buffers, paces (if `maxFps` set), and calls `AssetManager::update()`
for you each pass. `update` can run 0..`maxSteps` times per frame; input edges
are committed after each step that actually ran, so a press on a 0-step frame
is still delivered (exactly once) to the next step.

**B. Manual** — call `beginFrame(viewProj)` / `endFrame()` yourself
(e.g. to step the simulation deterministically for screenshots). `beginFrame`
returns `false` when the window was closed — then **do not** call `endFrame`.

Inside a frame, rendering is always the same contract:

```cpp
auto& batch = engine.batch();
batch.begin(viewProj);      // one or more times per frame
... draw ...
batch.end();                // flush
```

The batch stores its target framebuffer and viewport at `begin()`; drawing
into a `Framebuffer` and back to the screen is just `bind` → `begin`/`end` →
`Framebuffer::unbind` (see 04-graphics.md).

## Ownership and lifetime rules

| Object | Copyable? | Lifetime notes |
|---|---|---|
| `Engine` | no | must outlive everything that references its subsystems; one per thread |
| `Window` | move-only | owned by `Engine`; use `engine.window()` |
| `Texture2D`, `Font`, `Framebuffer`, `Shader` | move-only | GL objects; destroy with the class; `AssetManager` returns stable pointers until `clear()`/`unloadTexture` |
| `SpriteBatch` | no | owned by `Engine` |
| `PhysicsWorld::Body*` | — | pointer aliases internal vector; invalid after `addBody`/`removeBody`. Re-fetch with `get(id)` |
| ECS `Entity` | value | generational handle; stale handles fail `alive()` safely |
| `SoundHandle` | value | plain id; `AudioEngine` owns the decoded data |
| `ParticleSystem`, `Tilemap`, `Scene`, `Tweens`, `Random` | value-ish | plain members; configure before use |

## Threading

**All Forge APIs are main-thread only** unless noted. Specifically:

- GL objects, the `SpriteBatch`, `Window` and `Engine::run*` must live on the
  thread that created the `Engine`.
- `AudioEngine` setters (`setMasterVolume` etc.) are lock-free atomics so the
  audio callback thread can read them, but `loadFile`/`play`/`stop` take an
  internal mutex and are intended to be called from the game thread — don't
  call them from a second game thread.
- `Profiler` uses `steady_clock` on the calling thread; zones are per-frame.

There is no job system, no worker pool, no async asset loading. Load assets
synchronously (the examples do exactly this).

## What Forge is *not*

- Not a rigid-body engine: unit masses, no rotation/torque, one solver pass
  (deep stacks jitter), discrete collision. Great for platformers/top-down.
- Not a scene editor / asset pipeline: textures, fonts and audio load from
  paths at runtime; the examples even synthesize assets procedurally.
- Not a UI toolkit: text, rects and sprites only. Build menus with those.
- Not cross-platform yet beyond Linux/macOS-capable SDL3+GL (Windows and
  Emscripten are on the roadmap).

## Where to look in the repo

```
include/forge/     public headers (forge.hpp umbrella)
src/               implementations, mirrored by module
third_party/       stb_image, stb_truetype, stb_image_write, miniaudio
examples/          01_hello … 08_snake — canonical usage reference
tests/             headless unit tests (ctest)
docs/agents/       these docs
```
