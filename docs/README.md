# Forge documentation

Agent-oriented reference for the Forge engine. Written to be read by both
humans and coding agents; every signature and behavior here was verified
against the source in this repository (engine version 0.2.0).

If you only read one file, read [`agents/10-pitfalls.md`](agents/10-pitfalls.md).
If you are new to the codebase, start with
[`agents/00-overview.md`](agents/00-overview.md) and
[`agents/01-getting-started.md`](agents/01-getting-started.md).

## The set

| Doc | What it answers |
|---|---|
| [`agents/00-overview.md`](agents/00-overview.md) | What is Forge? How is it structured? What are the world/coordinate/memory conventions? What happens each frame? |
| [`agents/01-getting-started.md`](agents/01-getting-started.md) | How do I build it and get a window with moving things on screen? Which loop style do I need? |
| [`agents/02-core.md`](agents/02-core.md) | `Engine` configuration, window, timing, input (keys/mouse/gamepads/touch/IME/actions/remaps), logging, profiling |
| [`agents/03-math.md`](agents/03-math.md) | All math types and helpers: vectors, rects, colors, RNG, easing, noise, narrowphase |
| [`agents/04-graphics.md`](agents/04-graphics.md) | Batching, blend modes, scissor, cameras, textures, fonts (UTF-8/BMFont), offscreen + MSAA targets, tilemaps (multi-tileset/infinite/slopes), atlas packing, lighting, post-processing, skeletons, raw GL |
| [`agents/05-animation-particles.md`](agents/05-animation-particles.md) | Sprite animation, tweens, particle systems |
| [`agents/06-physics.md`](agents/06-physics.md) | The arcade physics world: bodies, filtering, contacts, sensors, one-way platforms, carry, joints, CCD, raycasts, debug draw |
| [`agents/07-scene-ecs.md`](agents/07-scene-ecs.md) | Entity/component registry, the `Scene` pipeline, versioned scene save/load and prefabs |
| [`agents/08-audio-assets.md`](agents/08-audio-assets.md) | Playing/decoding audio (including synthesized sounds and music crossfades), asset cache, search paths, shader/texture/font hot reload |
| [`agents/09-recipes.md`](agents/09-recipes.md) | Copy-paste solutions: HUD, camera follow, pause, grid games, headless capture, CI, … |
| [`agents/10-pitfalls.md`](agents/10-pitfalls.md) | Every known trap, its symptom, and the fix |
| [`agents/11-scripting-tools.md`](agents/11-scripting-tools.md) | `forge::script` (`.fgs`), `forge::json`, `DebugOverlay`, and how they compose in `10_lantern` |

## Suggested reading paths

- **Building a game on Forge** — 00 → 01 → 09, with 02-08 as lookup.
- **Writing engine code / fixing an engine bug** — 00 → 02 → 04 → 10, then
  the headers in `include/forge/`.
- **Debugging a broken game** — 10 first, then the module page involved.
- **Verifying behavior without a display** — 09 §Headless capture, plus
  `tests/test_engine.cpp` for how the headless unit tests are structured.

## Conventions used in these docs

- Code snippets compile against `include/forge/` as-is; they assume
  `using namespace forge;` is **not** active (everything is fully qualified or
  locally aliased, like the examples).
- "0-based" vs "1-based": tile ids are 1-based (`0` = empty); entity ids are
  generational handles (`0` = null).
- Signatures are copied from headers; behavior notes were verified in `src/`.
- The docs track version 0.2.0 — if the headers disagree, the headers win;
  update the docs in the same change.
