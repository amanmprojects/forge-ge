# Changelog

All notable changes to Forge. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the project is
pre-1.0, so breaking changes are allowed and are listed here.

## [Unreleased]

### Added

- Browser builds of all five games: `02_platformer`, `07_breakout`,
  `08_snake`, `09_chess` and `10_lantern` each get a page under
  `web/dist/<game>/`, generated from one template (`web/shell.html.in`) by
  `forge_web_page()` in `CMakeLists.txt`, plus a gallery at
  `web/dist/index.html`. Published to GitHub Pages by `web/deploy.sh`.
- `--record FILE --frames N [--size WxH]` on the five games: the autopilot
  runs one fixed step per frame, rendered offscreen and piped to `ffmpeg` as a
  60 fps H.264 MP4 (`examples/common/clip.hpp`). `web/record.sh` records the
  gallery clips.
- `02_platformer --demo`: an autopilot that climbs the three platforms; the
  example is now a `Platformer` struct with a render-to-target path.

### Changed

- `Engine::quit()` (and `quitOnEscape`) is a no-op on the web. Before, Q in
  the chess page cancelled the main loop and froze the canvas.
- The chess page moved from `web/dist/` to `web/dist/chess/`.
- The chess bot's inline pacing is a runtime setting (`setBotPace`), on for
  the web build and `--record`, off for `--shot`/CI as before.

- The web build compiles with `-fwasm-exceptions`, and the web games keep
  the engine and game in function-local statics instead of `unique_ptr`
  owners (the exception-based unwind out of `main()` would destroy those).

### Fixed

- `PostProcess` on the web: its built-in passes declare `#version 330 core`,
  which WebGL2 rejects. `Shader::load` now swaps a leading desktop directive
  for the ES header on the web, for any source.
- `forge::script` on the web: the interpreter unwinds `return`/`break` with
  C++ exceptions, which aborted the page under Emscripten's default build.
- `10_lantern` rendered at its startup size after the window was resized, and
  its autopilot got pinned against the raised wall (it now follows a tile
  BFS path and lights every lantern).

## [0.2.0] — 2026-10-19

The "roadmap" release: 0.1.0 correctness debt plus every planned system in
`docs/ROADMAP.md`, still with **SDL3 + OpenGL as the only system
dependencies** (no ImGui, Lua, Box2D, Spine, or third-party JSON).

### Fixed (0.1.0 correctness debt)

- `EngineConfig::window` is now honoured — title, size, vsync, fullscreen and
  MSAA reach `Window::create`. Added `Engine::config()`, `Window::config()`
  and `Window::title()`.
- `SpriteBatch` scissor clipping (`setScissor`/`clearScissor`) with correct
  save/reset semantics across FBO passes.
- Font: UTF-8 decoding, on-demand multi-page glyph atlas, word wrapping,
  `measure/widthOf/heightWrapped/widthWrapped`, `drawWrapped*`, and a `?`
  fallback for missing glyphs.
- `Framebuffer`: MSAA (`create(w, h, samples)`), `resolve()`, `resize()`,
  `readPixels()`, `samples()/multisampled()`.
- Audio: `crossfadeMusic()`, `musicFadeProgress()`, and `stopMusic()` stopping
  fading voices.
- `LightingSystem`: the composite no longer mirrors the lightmap vertically
  (it sampled V flipped; both the lightmap and the target are framebuffers with
  a bottom-left origin). Caught by the new `06_verify` `lighting-placement`
  check, which renders an off-center light and asserts the matching pixel.
- `LightingSystem`: flicker is evaluated at draw time instead of multiplying
  `Light2D::intensity` in place on every `update()`. The old code compounded the
  modulation, so lights slowly faded (or blew out) and re-adding lights each
  frame was order-dependent. `intensity` now always holds the authored value.

### Added — graphics

- `SpriteBatch`: blend modes (`Alpha`, `Additive`, `Multiply`, `None`),
  `drawQuad`, `drawNineSlice`, `drawPanel`, `drawTextShadow`; explicit state
  reset/restore around passes.
- `Texture2D::updateRegion` (sub-uploads for the glyph atlas) and
  `adoptRaw(id, w, h, owns)` for wrapping framebuffer textures without
  double-owning the GL id.
- `Tilemap`: multiple tilesets (`firstgid` per set, index packed in the high
  16 bits), Tiled flip/diagonal flags, infinite maps (chunked data expanded to
  dense bounds + `originOffset()`), object layers (`TilemapObject`), per-layer
  offset/opacity/visibility/parallax, autotiling (`setAutoRule`,
  `applyAutotile`, `buildAutoRule`), and slopes (`setSlope`, `slope`,
  `slopeSurfaceHeight`, `surfaceYAt`) resolved vertically by `collideRect`.
- `AtlasPacker` — CPU shelf/skyline packer with padding, `blit`, occupancy.
- `LightingSystem` — ambient + point lights accumulated into a lightmap,
  deterministic flicker, optional shadow occluders, and an optional target
  framebuffer for offscreen pipelines.
- `PostProcess` / `FullscreenPass` / `RenderTargetStack` — vignette/scanline
  preset, custom fullscreen shaders, and pooled nested render targets.
- `Skeleton` — bones + slots + keyframe tracks (translation/rotation/scale/
  attachment) loaded from JSON, with `debugDraw`.
- `Font::loadBitmapFnt` — BMFont text format + PNG pages, no stb_truetype.

### Added — physics (still arcade-grade)

- Layer/mask collision filtering (`Body::layer`, `Body::mask`).
- Trigger enter/exit events for sensors (`onTriggerEnter`, `onTriggerExit`).
- One-way platforms (`Body::oneWay`) with a prev-position test.
- Moving-platform carry for riders.
- Circle colliders (`ShapeType::Circle`) with circle-circle/circle-box MTV.
- Distance and spring joints (`addJoint`, `removeJoint`).
- CCD substepping derived from speed vs. body size (capped at 16).
- `PhysicsWorld::debugDraw` (bodies by type, sensors, joints, contacts,
  velocities, labels).

### Added — gameplay systems and tools

- `forge/utils/json.hpp` — ordered, header-only JSON value/parser/writer with
  atomic `writeFile`, full escape handling (incl. surrogate pairs), and no
  exceptions.
- Scene serialization and prefabs: `saveScene`/`loadScene` (versioned
  `forge.scene` v1), `makePrefab`/`savePrefab`/`loadPrefab`/`instantiate`,
  `entityToJson`/`entityFromJson`, and a `ScriptRef` component that is
  serialized while `Script` closures are re-bound by name.
- `forge::script` — a small deterministic interpreter (`.fgs`): numbers,
  bools, strings, null, ordered tables, `var`, `if`/`else`, `while`, `for`,
  functions, host bindings, step budget, hot reload. Errors never throw.
- `DebugOverlay` — dependency-free F1 overlay: FPS graph with 1% low, frame
  zones, draw calls/quads, entity/body counts, log tail.
- Input depth: touch (down/move/up/cancel, latches, `touchDrag`), IME
  composition state, stick deadzone + response curve, and persisted action
  remaps (`serializeBinds`/`deserializeBinds`/`saveBinds`/`loadBinds`).
- `AssetManager`: search paths (`addSearchPath`/`resolve`), and
  `loadShader(vert, frag)` with mtime hot reload that keeps the previous
  program on a failed compile.
- `examples/10_lantern.cpp` — the integration example: tilemap + slope,
  lantern lighting, vignette post pass, `.fgs` hot reload (F2), scene save/load
  (F5/F9), debug overlay (F1), touch, `--shot`, `--selftest` and `CI=1`.
  Pickup no longer removes a *different*, still-alive lantern from the tracked
  list (the collect path used to erase from the list it had just rebuilt).

### Added — platform and distribution

- CMake install + export: `find_package(forge)` and the `forge::forge`
  target, with a generated config/version file.
- `FORGE_PLATFORM_*` macros and Emscripten/WebGL2 build groundwork (see
  `web/README.md`).
- `.github/workflows/ci.yml` — Linux GCC/Clang (Release + Debug/ASan), macOS,
  Windows; tests, smoke runs, screenshot determinism, and a consumer package
  check.
- `tests/test_docs.cpp` — compile-time API drift guard for documented
  signatures.
- Expanded `06_verify` (scissor, blend modes, MSAA resolve, post pass and its
  orientation, lighting falloff *and* placement, UTF-8) and
  `tests/test_engine.cpp` (JSON, scripting, atlas, serialization, font,
  input depth, slopes, multi-tileset/infinite/objects/autotile tilemaps,
  moving-platform carry, lighting bookkeeping).
- `10_lantern --selftest` — a headless save/load round-trip (save, mutate,
  reload, compare) so the F5/F9 path is covered by CI, and `--overlay` to
  include the debug panel in a `--shot` capture.

### Changed

- Project version `0.2.0`; the umbrella header's fallback version matches.
- `LightingSystem::render` and `PostProcess::apply` take/behave with explicit
  framebuffer targets; `flipY` defaults to `false` (framebuffer sources share
  the target's bottom-left origin).
- Physics `Body` gained `layer`/`mask`/`sensor`/`oneWay`/`shape`/`radius`;
  defaults preserve 0.1.0 behavior.

### Docs

- `docs/ROADMAP.md` (living plan with acceptance criteria), new
  `docs/agents/11-scripting-tools.md`, and updates to every existing
  `docs/agents/*` page, `README.md` and `AGENTS.md`.

## [0.1.0] — 2025

- First release: `Engine` (fixed/variable loops), SDL3 window, batched
  OpenGL 3.3 renderer, camera, textures, TTF text, framebuffer, particles,
  tweens, tilemaps, arcade AABB physics, ECS `Scene`, audio mixer, asset cache
  with hot reload, profiler, and the first examples (hello, platformer,
  benchmark, audio, scene, verify, breakout, snake, chess).
