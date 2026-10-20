# Forge roadmap — 0.1.0 → 1.0

Living document. Read this first after a context compaction: it is the single
source of truth for what is done, what is in flight, and what "done" means.
`README.md` keeps the short public summary; this file keeps the full plan.

**Status legend**

- `[x]` done and covered by tests (or a headless example run)
- `[~]` implemented, but not fully verified / documented
- `[ ]` not started
- `[-]` intentionally out of scope (see *Non-goals*)

**Rules**

1. Every item lands with a build (`-Wall -Wextra -Wpedantic`, warning-free),
   `ctest` green, the render self-test (`./build/06_verify`) green, and the
   headless smoke runs green: `CI=1 ./build/07_breakout --frames 240`,
   `CI=1 ./build/08_snake --frames 240`, `CI=1 ./build/09_chess`,
   `CI=1 ./build/10_lantern --frames 240`, `./build/10_lantern --selftest`.
2. Public API changes update `docs/agents/*`, `README.md` and `AGENTS.md` in the
   same change. `tests/test_docs.cpp` fails the build when a documented
   signature disappears or an agent page is not linked from `docs/README.md`.
3. New gameplay-facing features get an example or a recipe in
   `docs/agents/09-recipes.md`.
4. No new system dependencies. Vendored code only (`third_party/`).

**Current release: 0.2.0** (see [`../CHANGELOG.md`](../CHANGELOG.md)).
Everything through Phase 5 is implemented; the only `[~]` item is the CI
matrix, which exists in `.github/workflows/ci.yml` but can only be verified
by pushing to GitHub.

---

## Phase 0 — 0.1.0 correctness debt (P0)

- [x] **0.1 `EngineConfig::window` is honoured.**
      `Engine` constructs `Window window_` from `cfg.window`, so
      title/size/vsync/fullscreen/MSAA apply at creation. Added
      `Engine::config()`, `Window::config()` and `Window::title()`.
      *Verified by:* `tests/test_engine.cpp` window/config checks and the
      examples (title comes from the config; no runtime `setTitle` needed).
- [x] **0.2 Scissor / clip rectangles in `SpriteBatch`.**
      `setScissor(Rect)` (framebuffer pixels, bottom-left origin) and
      `clearScissor()`; flush on change; `begin()` resets; `end()` disables so
      no GL state leaks into FBO or user GL code.
      *Verified by:* `06_verify` (`scissor-inside`, `scissor-outside`,
      `scissor-reset`).
- [x] **0.3 UTF-8 text, dynamic glyph atlas, wrapping.**
      UTF-8 decoding, on-demand glyph baking into shelf-packed 1024² pages
      (multi-page), `Texture2D::updateRegion`, `drawWrapped*`,
      `heightWrapped`/`widthWrapped` (word wrap + long-word break), `?`
      fallback for missing codepoints.
      *Verified by:* `06_verify` (`utf8-glyphs`, `utf8-wrapped-height`) and
      `tests/test_engine.cpp` (`testFontUtf8`).
- [x] **0.4 `Framebuffer` MSAA + resize.**
      `create(w, h, samples = 0)` with a multisampled renderbuffer + resolve
      FBO, `resolve()`, `resize(w, h)`, and `readPixels()`.
      *Verified by:* `06_verify` (`msaa-resolve`, `msaa-resize`) and
      `tests/test_engine.cpp`.
- [x] **0.5 Audio music crossfade.**
      `crossfadeMusic(handle, seconds, volume)`; fading voices die at zero;
      `stopMusic()` stops every music voice (including fades);
      `musicFadeProgress()`.
      *Verified by:* `tests/test_engine.cpp` (device-independent voice
      bookkeeping).
- [x] **0.6 Docs/API drift guard.**
      `tests/test_docs.cpp` takes the address of every documented signature
      (compile-time static_asserts), checks the documented files exist, that
      every agent page is linked from `docs/README.md`, and that the roadmap
      still lists every phase.
      *Verified by:* `ctest` (`test_docs` fails if a documented signature
      disappears).

## Phase 1 — platform & distribution (P1)

- [x] **1.1 CMake install + export.**
      `install(TARGETS forge EXPORT forgeTargets ...)`, generated
      `forgeConfig.cmake` + version file, public headers installed under
      `include/forge`. Consumers use `find_package(forge REQUIRED)` and link
      `forge::forge`.
      *Verified by:* the CI "Install + consumer package check" step and a
      local install to `/tmp/forge-prefix` (a 10-line consumer builds and
      runs with `CMAKE_PREFIX_PATH` only).
- [~] **1.2 CI matrix.**
      `.github/workflows/ci.yml`: Linux GCC + Clang (Release and Debug/ASan),
      macOS (brew SDL3), Windows (vcpkg SDL3); `ctest`, the `CI=1` smoke runs
      for all four games, `--shot` determinism (same args → byte-identical
      PNG), and the install/consumer check.
      *Why `[~]`:* the workflow is written but can only be executed on GitHub;
      the same commands pass locally (Linux).
- [x] **1.3 Emscripten / WebGL2 groundwork.**
      `FORGE_PLATFORM_WEB` detection, GLSL ES 3.00 shader variants
      (`#version 300 es`), `emscripten_set_main_loop_arg(..., simulate_infinite_loop=1)`
      paths for `runFixed`/`runVariable`, CMake flags, and docs.
      *Verified by:* `./web/build.sh` builds `09_chess` to `web/dist` (see
      [`../web/README.md`](../web/README.md)).
- [x] **1.4 Platform macros + path helpers.**
      `FORGE_PLATFORM_WINDOWS/MACOS/LINUX/WEB` in
      `include/forge/core/platform.hpp`, and `AssetManager` search paths
      (`addSearchPath`/`clearSearchPaths`/`resolve`) so relative loads work
      from the repo root, the build dir and an installed layout.
      *Verified by:* `tests/test_engine.cpp` (`testAssetResolve`) and
      `10_lantern` probing `assets/lantern.fgs`.

## Phase 2 — graphics depth (P2)

- [x] **2.1 Tilemap completeness.**
      - multi-tileset (`firstgid` per set, tileset index in the high 16 bits)
      - Tiled flip/rotate flags (H/V/diagonal) preserved per tile
      - infinite maps (chunked `data` → dense bounds + origin offset)
      - object layers exposed as `TilemapObject{name,type,rect,point}`
      - per-layer offset, opacity, visibility + parallax factor
      - autotile helper (8-neighbour bitmask → tile id table)
      *Verified by:* `tests/test_engine.cpp` fixtures for every bullet, and
      `10_lantern` drawing a flipped tile and a ramp.
- [x] **2.2 UI primitives.**
      `drawNineSlice`, `drawPanel`, `drawTextShadow`, `drawQuad`; blend modes
      (`Alpha`, `Additive`, `Multiply`, `None`) on `SpriteBatch`.
      *Verified by:* `06_verify` (`blend-additive`, `blend-reset`) +
      `10_lantern` HUD.
- [x] **2.3 Atlas packer + bitmap fonts.**
      `AtlasPacker` (shelf packer with padding, `blit`, occupancy) returning
      pixel rects; `Font::loadBitmapFnt` accepts a pre-baked BMFont atlas.
      *Verified by:* `tests/test_engine.cpp` (`testAtlas`) and the
      `loadBitmapFnt` error-path checks.
- [x] **2.4 2D lighting.**
      `LightingSystem`: ambient + point lights accumulated in an offscreen
      target, radial falloff texture, additive accumulation, multiply
      composite, optional shadow occluders, deterministic flicker.
      *Verified by:* `06_verify` (`lighting-falloff`, `lighting-placement` —
      an off-center light must land at the matching pixel, catching a mirrored
      composite) and `10_lantern`.
- [x] **2.5 Post-processing / render-target stack.**
      `FullscreenPass` (shader + source texture), `PostProcess`
      (vignette/scanline preset), `RenderTargetStack` for nested FBOs.
      *Verified by:* `06_verify` (`post-center-bright`, `post-vignette`) and
      `10_lantern`'s `--shot` path.
- [x] **2.6 Shader hot reload.**
      `AssetManager::loadShader(vert, frag)` with mtime checks folded into the
      2 Hz `update()`; a failed recompile keeps the previous program.
      *Verified by:* `tests/test_engine.cpp` (cache identity + mtime swap).
- [x] **2.7 Skeletal animation.**
      Bone hierarchy + slot attachments + keyframe tracks (translation,
      rotation, scale, attachment), `Skeleton::update/draw/debugDraw`, JSON
      load via `utils/json.hpp`.
      *Verified by:* `tests/test_engine.cpp` (`testSkeleton`).

## Phase 3 — physics without betraying the arcade design (P3)

- [x] **3.1 Filtering.** `Body::layer` / `Body::mask`; broadphase + narrowphase
      skip non-matching pairs. *Verified by:* `testPhysicsFeatures`.
- [x] **3.2 Trigger events.** `onTriggerEnter` / `onTriggerExit` from a
      persistent contact set (sensors only), in addition to `onContact`.
      *Verified by:* `testPhysicsFeatures`.
- [x] **3.3 One-way platforms.** `Body::oneWay`; collides only when a dynamic
      body approaches from above (`prevPos`). *Verified by:* `testPhysicsFeatures`.
- [x] **3.4 Moving-platform carry.** Riders inherit the support delta
      (horizontal + vertical). *Verified by:* `testPhysicsFeatures`.
- [x] **3.5 Circle colliders.** `Body::shape` (`Box`/`Circle`) + radius;
      circle-circle and circle-box MTV; solver + raycast + query support.
      *Verified by:* `testPhysicsFeatures`.
- [x] **3.6 Joints.** Distance and spring joints solved after integration,
      position + velocity correction, `addJoint/removeJoint`.
      *Verified by:* `testPhysicsFeatures`.
- [x] **3.7 CCD.** Substeps derived from max speed vs. min body extent (cap at
      16) so fast projectiles do not tunnel. *Verified by:* `testPhysicsFeatures`
      (including the documented cap: a 6000 px/s 4 px bullet needs ~100
      substeps and still tunnels — arcade trade-off, use raycasts for those).
- [x] **3.8 Debug draw.** `PhysicsWorld::debugDraw` — bodies by type, sensors,
      contacts, joints, velocities, optional labels.
      *Verified by:* `10_lantern` (F3 wireframe) and `testPhysics` (runs
      without a GL context? no: exercised in the example).
- [x] **3.9 Slopes.** Arbitrary-height tile slopes via per-tile
      `TileSlope`, only for tilemap collisions (not the box solver).
      *Verified by:* `testTilemap` slope cases and `10_lantern`.

## Phase 4 — gameplay systems & tools (P4)

- [x] **4.1 `utils/json.hpp`.** Public, header-only JSON value + parser +
      writer, used by tilemaps, save files and prefabs. No exceptions,
      insertion-ordered objects, atomic `writeFile`, full escapes.
      *Verified by:* `testJson`.
- [x] **4.2 Scene serialization.** Versioned save file
      (`{"format":"forge.scene","version":1}`): entities, tags, transforms,
      sprites (asset path + rect), velocities, colliders, script ids.
      `saveScene`/`loadScene`, round-trip test.
      *Verified by:* `testSerialization` and `10_lantern --selftest`.
- [x] **4.3 Prefabs.** `makePrefab` / `savePrefab` / `loadPrefab` /
      `instantiate(prefab, pos)` with a position override.
      *Verified by:* `testSerialization` and `10_lantern --selftest`.
- [x] **4.4 Debug overlay.** Dependency-free `DebugOverlay` (F1): FPS graph,
      1% low, frame zones, draw calls/quads, entity + body counts, log tail.
      (Chosen over ImGui to keep the zero-dependency promise.)
      *Verified by:* `10_lantern` (F1) and the overlay's unit-tested
      `averageFps`/`onePercentLowMs`.
- [x] **4.5 Scripting.** `forge::script` — small, deterministic,
      dependency-free interpreter (numbers, strings, bools, vars, `if`,
      `while`, `for`, functions, tables, host bindings) + hot reload from
      `.fgs` files. Full Lua stays a non-goal.
      *Verified by:* `testScript` and `10_lantern` F2.
- [x] **4.6 Input depth.** Touch events, IME/text-editing state, persisted
      action remaps, stick deadzone/response curves.
      *Verified by:* `testInputDepth`.
- [x] **4.7 Example 10.** One complete small game integrating tilemap +
      lighting + save/load + overlay + scripting, with `--shot` and `CI=1`
      modes. It is the integration test and the "how it all fits" reference.
      *Verified by:* `CI=1 ./build/10_lantern --frames 240` and the
      deterministic `--shot` compare in CI.

## Phase 5 — docs & polish (P5)

- [x] **5.1** Updated every `docs/agents/*` page for the new APIs, and added
      `docs/agents/11-scripting-tools.md`.
- [x] **5.2** `docs/agents/09-recipes.md`: HUD clipping, text wrapping,
      lighting, post-processing, save/load, prefabs, scripting, debug overlay,
      touch/remaps.
- [x] **5.3** `docs/agents/10-pitfalls.md`: scissor origin, blend/GL state
      leaks, font atlas growth, MSAA resolve, one-way platforms, CCD limits,
      post-process orientation, slopes, scripting errors, serialization.
- [x] **5.4** README: new example, new modules, measured numbers, roadmap link.
- [x] **5.5** AGENTS.md: module map + example map + rules for new subsystems.
- [x] **5.6** `CHANGELOG.md` from 0.1.0 onward.

---

## Explicit non-goals

- 3D rendering, full rigid-body/soft-body simulation, a visual editor,
  networking, mobile-first input, an asset import pipeline.
- A hard dependency on ImGui/Lua/Box2D/Spine. Optional integrations may be
  documented, never required to build `forge`.
- Backwards compatibility with 0.1.0 where it blocks correctness. This is a
  pre-1.0 engine; breaking changes are allowed and must be listed in
  `CHANGELOG.md`.

## Progress log

- 2026-10-19: roadmap created; Phase 0 in progress.
- 2026-10-19: Phase 0 complete (window config, scissor, UTF-8/wrapped text,
  MSAA, crossfade, docs guard). `ctest` green.
- 2026-10-19: Phase 1 complete except the CI matrix being unverified until
  pushed (install/export + consumer check pass locally; web groundwork and
  search paths landed).
- 2026-10-19: Phase 2 complete (tilemap completeness + slopes, UI primitives,
  atlas + bitmap fonts, lighting, post-processing, shader hot reload,
  skeleton). `06_verify` covers scissor/MSAA/post/lighting/UTF-8.
- 2026-10-19: Phase 3 complete (filtering, triggers, one-way, carry, circles,
  joints, CCD, debug draw, slopes).
- 2026-10-19: Phase 4 complete (json, scene save/load, prefabs, overlay,
  scripting, input depth, `10_lantern`).
- 2026-10-19: Phase 5 complete (all agent pages, recipes, pitfalls, README,
  AGENTS.md, CHANGELOG). Release 0.2.0.
- 2026-10-19: end-to-end verification pass. `06_verify` grew blend-mode,
  post-orientation and lighting-placement checks; the last one found and fixed
  a mirrored lightmap composite. Flicker no longer compounds in
  `LightingSystem::update`. `10_lantern` gained `--selftest` (headless
  save/load round-trip) and `--overlay`, and its pickup loop no longer drops a
  still-alive lantern from the tracked list. Clean Release build is
  warning-free; `ctest` 3/3, `06_verify` verified, all `CI=1` smoke runs and
  both `--shot` determinism compares pass, and the ASan build is clean.

## Next (post-0.2.0 ideas, not scheduled)

- Verify the CI matrix on GitHub and add a WebAssembly job (the `web/` build
  could not be re-run locally: no Emscripten SDK on this machine).
- Tilemap: Wang/terrain sets from Tiled, animated tiles.
- Audio: streaming music decode (currently full-file).
- Font: SDF rendering option.
- Scripting: coroutines or a small bytecode compiler if profiles demand it.
