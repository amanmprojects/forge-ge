# 10 — Pitfalls and invariants

Every entry: **symptom → cause → fix**. Check here before debugging; this list
comes from the actual implementation, not the marketing.

## Engine and the frame loop

### 1. `EngineConfig::window` does nothing (0.1.0)

**Symptom:** `cfg.window.title/width/height/vsync` have no effect; the log says
`Window 'Forge Game' 1280x720`.
**Cause:** `Engine` default-constructs its `Window` member before applying the
config, so `cfg.window` is never passed to `Window::create`.
**Fix:** call `engine.window().setTitle("…")` after construction. Everything
else in `EngineConfig` (`fixedStep`, `maxSteps`, `initAudio`, `quitOnEscape`,
`maxFps`) works. Window is resizable + vsync-on by default.

### 2. `endFrame()` after a failed `beginFrame`

**Symptom:** crash/garbage or an unbalanced profiler when the window closes.
**Cause:** `beginFrame` returns `false` for the quit event and does *not* begin
a batch or a profiler frame.
**Fix:** `if (!engine.beginFrame(vp)) break;` — never call `endFrame` on that
path.

### 3. Manual loops skip hot reload and pacing

**Symptom:** edited assets never reload; `maxFps` ignored.
**Cause:** `runFixed`/`runVariable` call `assets_.update()` and `pace()`; a
manual `beginFrame`/`endFrame` loop does neither.
**Fix:** call `engine.assets().update()` yourself if you rely on hot reload,
and pace manually (e.g. `SDL_Delay`) if you need a frame cap.

### 4. Fixed-step update ran zero times this frame

**Symptom:** "first frame" logic in `update` never fires, or runs multiple
times in one visible frame; physics "pauses" for a frame.
**Cause:** `runFixed` calls `update` 0..`maxSteps` times per rendered frame,
and `render` exactly once.
**Fix:** put simulation in `update` and rendering in `render`; never put
"once per frame" UI logic in `update`. Use `Time::frame()` if you need a
frame counter.

### 5. `Engine::clear` cleared the wrong thing

**Symptom:** the screen stays black while an offscreen buffer gets cleared
(or vice versa).
**Cause:** `clear()` clears the **currently bound** framebuffer with the window
viewport.
**Fix:** `Framebuffer::unbind(...)` before the screen clear, or bind the FBO
first and clear deliberately (that's how `--shot` capture works).

## Coordinates, textures, rendering

### 6. Mouse/world mismatch

**Symptom:** clicks land mirrored vertically or offset by half the screen.
**Cause:** mouse coords are top-left SDL pixels; the world is Y-up.
**Fix:** `cam.screenToWorld(in.mousePos(), vp.x, vp.y)` — and remember it
includes zoom, rotation and the current shake offset.

### 7. Texture appears upside down / atlas regions wrong

**Symptom:** a hand-uploaded texture is flipped, or `drawTextureRegion` shows
the wrong part of a sheet.
**Cause:** `loadFile` defaults to `flipY = true` (UV `(0,0)` = image
bottom-left) and the pixel-rect helpers assume that convention. `loadMemory`
uploads exactly the rows you give it (row 0 becomes the texture bottom).
**Fix:** when hand-painting pixels for use with pixel-rect helpers, flip rows
before upload (see 09-recipes "Procedural textures"). For full `SpriteDraw`
control, set the UV `src` yourself — it is not converted.

### 8. `SpriteDraw::src` is UV, everything else is pixels

**Symptom:** huge/wrong atlas region or a 1-pixel smear.
**Cause:** `SpriteDraw.src` is a 0..1 UV rect; `drawTextureRegion`, `Sprite::src`,
`AnimClip` frames and `Tilemap` tiles are **pixel** rects.
**Fix:** use the region helper or convert:
`{px.x/tw, 1-(px.y+px.h)/th, px.w/tw, px.h/th}`.

### 9. Sprite is offset after rotating

**Symptom:** rotation swings the sprite around an unexpected point.
**Cause:** `SpriteDraw.pos` is the quad **center** and `origin` is a normalized
pivot (default center).
**Fix:** set `origin = {0,0}` (bottom-left pivot) or
`d.pos = intendedCenter - (desiredPivot - 0.5) * size`.

### 10. Un-ended batch / draw calls outside begin-end

**Symptom:** a `FORGE_LOG_WARN` about "previous batch was not ended", quads
landing in the wrong target, or shapes missing entirely.
**Cause:** `draw()` only stages vertices; without `end()` nothing flushes.
`begin()` flushes a leftover batch into its *old* framebuffer/viewport.
**Fix:** treat `begin`/`end` as a mandatory pair; never submit draws between
`end()` and the next `begin()`.

### 11. Draw-call count higher than expected

**Symptom:** `drawCalls()` ≈ number of alternations, not 1.
**Cause:** flushes at 8192 quads, at 17 distinct textures, at `begin`, and at
`end`. The white texture (shapes/text) also occupies a slot.
**Fix:** sort/group by texture within a pass; keep ≤ 16 textures per pass;
prefer atlas sheets over many small textures. `03_benchmark` shows the ideal.

### 12. Text not showing / `?????` / tiny

**Symptom:** `font.draw` does nothing, shows latin-1 fallback `'?'`, or text
is not where you expect.
**Cause:** font failed to load (missing file or atlas overflow at large
`pixelHeight`), non-ASCII input (atlas is 32–126 only), or `pos` semantics
(top-left of the first line, Y-up), or you forgot the HUD pass'
`batch.begin(engine.defaultProj())`.
**Fix:** check `font.valid()`/`loadFile` return, keep `pixelHeight` ≤ ~40,
use ASCII, and draw in a screen-space pass. `measure()`/`widthOf()` help
positioning.

### 13. Captured screenshots are upside down

**Symptom:** a PNG written from `glReadPixels` is vertically flipped (or a
screenshot-test hash never matches).
**Cause:** OpenGL readback returns rows **bottom-up** (row 0 = window bottom);
PNG expects top-down.
**Fix:** flip the rows before `stbi_write_png` — see the `saveScreenshot`
helper in 09-recipes and both example games. Note this is purely a readback
detail: a `Framebuffer` texture drawn through the batch samples upright with
default `src`.

### 14. Tilemap renders nothing / wrong tiles / invisible walls

**Symptom:** blank map, offset tiles, or walls that don't collide.
**Cause:**
- `setTileset()` not called (needed before `draw`),
- Tiled export uses a second tileset, packed/tiled data, or tile flip flags
  (only the first tileset is read; flip flags are masked; infinite maps unsupported),
- CSV row order confused (row 0 = bottom in Forge, top in most editors),
- solids set on ids that don't match the layer you think (`setSolid` is
  global per id), or layer visibility toggled expecting collision changes.
**Fix:** verify with `tile(0,x,y)`, log `tilesetPath()`, and remember
**visibility is rendering only**; `solidsInRect`/`collideRect` ignore it.

### 15. `collideRect` leaves the player jittering inside walls

**Symptom:** multi-tile overlaps behave oddly; tunneling at high speed.
**Cause:** each tile contributes its own MTV, and the accumulated `resolve`
applied once can overshoot/cancel near corners; the query is discrete.
**Fix:** apply `resolve`, then run `collideRect` again with the corrected rect
(two iterations resolve single-layer walls); clamp speed or substep movement
for fast movers.

## Math

### 16. `Rect` is not centered

**Symptom:** AABB checks offset by half size.
**Cause:** `Rect{x,y,w,h}` uses `(x,y)` as the **min corner** (bottom-left,
Y-up), not the center.
**Fix:** `Rect::fromCenter(center, size)` when you have a center (that's what
`Body::aabb()` uses).

### 17. Overlap checks say "no" when touching

**Symptom:** items exactly resting on a surface count as "not overlapping".
**Cause:** `Rect::overlaps` is strict (`<`/`>`), while `Rect::contains` is
inclusive.
**Fix:** `expanded(skin)` for a tolerance; don't mix the two.

### 18. `Color::fromHex` order

**Symptom:** alpha/blue channels swapped in a hex color.
**Cause:** the format is `0xRRGGBBAA` (alpha is the last byte).
**Fix:** pad with `FF` for opaque: `0xE9C46AFF`. It is not `0xAARRGGBB`.

### 19. RNG not reproducible

**Symptom:** two runs with the "same" seed differ.
**Cause:** `sharedRandom()` is seeded from `std::random_device`; `Random::seed(0)`
is coerced; engines that consume RNG for visuals (popup jitter, particles) change
spawns when flags change — that's by design but surprises.
**Fix:** use a seeded `forge::Random` member for anything that must replay;
seed particle systems explicitly (`EmitterConfig::seed`).

### 20. Integer division for tiles with negative coordinates

**Symptom:** off-by-one tile lookups to the left/below the origin.
**Cause:** `int / int` truncates toward zero in C++.
**Fix:** `(int)std::floor(worldX / tileW)` — the engine's tile code does this
everywhere; do the same in your game.

## Input

### 21. `pressed()` true while `isDown()` false

**Symptom:** code that combines `isDown && pressed` misses fast taps.
**Cause:** press/release **latches** survive sub-frame taps on purpose (a tap
that went down and up between two `beginFrame` calls reports both edges).
**Fix:** use `pressed()` alone for edge actions; don't require `isDown()` at
the same time.

### 22. `moveAxis()` reads every connected gamepad

**Symptom:** with two pads connected, movement merges/amounts double.
**Cause:** default `pad = -1` means "all connected pads".
**Fix:** pass a specific pad (`moveAxis(0)`) for single-player, or accept the
merge for couch co-op.

### 23. Key enum order is not alphabetical

**Symptom:** wrong key fires when looping/adding over the enum.
**Cause:** letters are grouped oddly (`Q,E,R,F,Z,X,…` after the F-keys).
**Fix:** always reference keys by name (`Key::A`); use `keyFromName()` for
config strings.

### 24. ESC doesn't quit by default

**Symptom:** pressing Escape does nothing.
**Cause:** `quitOnEscape` defaults to `false` and ESC isn't otherwise bound.
**Fix:** set `cfg.quitOnEscape = true`, or handle `Key::Escape` in your update
(most games use ESC for pause instead).

## Physics

### 25. `Body*` pointer goes stale

**Symptom:** corrupted velocities or crashes after spawning/removing bodies.
**Cause:** `get()` returns a pointer into a vector that `addBody`/`removeBody`
reallocate.
**Fix:** store the body **id**, re-fetch each frame (`world.get(id)`), re-fetch
after any structural change.

### 26. Everything slides down in a top-down game

**Symptom:** sprites fall off the screen the moment they get a Collider.
**Cause:** default gravity is `(0, -980)` — fine for platformers, wrong for
top-down.
**Fix:** `world.setGravity({0,0})` (or `scene.physics().setGravity({0,0})`).

### 27. Sensors still collide

**Symptom:** trigger volumes block movement.
**Cause:** `sensor = true` reports contacts but does **not** resolve; if it
still blocks, you set `solid = false` on a body used by code that treats
`!solid` as a wall, or you're colliding manually against `aabb()`.
**Fix:** sensors are one-way doors: only the solver skips them. Scene maps
`Collider::solid = false` to `sensor = true` automatically. Overlap tests you
write yourself must check `body.sensor`.

### 28. Contacts fire many times

**Symptom:** one landing plays the sound 3×, pickup triggers twice.
**Cause:** the callback fires per overlapping pair **per substep**, and
possibly on both bodies' ids.
**Fix:** debounce in game state (compare against last-frame `onGround`, or
track handled ids in a set for the frame).

### 29. Jittery stacks / deep overlaps

**Symptom:** boxes vibrate or slowly sink into each other.
**Cause:** single relaxation pass, no mass, no sleeping — by design.
**Fix:** design around it (tiles for level geometry, snap positions, wake/
sleep logic in game code) or don't stack dynamics.

### 30. Kinematic platform pushes the player through walls

**Symptom:** a moving kinematic body squeezes dynamic bodies through geometry.
**Cause:** kinematic-vs-dynamic uses infinite kinematic mass; the dynamic
body is displaced by the full penetration each substep.
**Fix:** move kinematics in small steps (set `vel`, not teleporting `pos`),
or make the platform route collide-space-safe; the solver has no continuous
detection.

## ECS / Scene

### 31. Crash inside `view<Ts>()`

**Symptom:** use-after-free/asserts when the callback destroys entities or
adds components.
**Cause:** structural changes mutate the dense arrays under iteration.
**Fix:** defer to after the view (see 09-recipes "ECS: safe structural
changes"). `Scene::update` scripts already get a snapshot; user views don't.

### 32. Wrong entity update cost / skipped entities

**Symptom:** iteration slower than expected or logic missing entities.
**Cause:** `view<Ts>()` iterates the pool of the **first listed component**
and skips entities missing the rest. (`Registry::view` is not a join
optimizer.)
**Fix:** list the rarest component first; prefer `tryGet` for optional data
rather than adding it to `Ts`.

### 33. `get<T>` asserts in debug builds

**Symptom:** crash on `registry.get<T>(e)` for entities that lost the
component.
**Cause:** `get` asserts `has<T>`; release builds skip the check and return
garbage.
**Fix:** `tryGet<T>` and null-check when presence isn't guaranteed.

### 34. Entity creation returns 0 after long sessions

**Symptom:** `create()` occasionally returns `kNullEntity`; new objects
invisible.
**Cause:** 20-bit index space exhausted (≈1M live+recycled slots; recycling
requires `destroy`, not just letting handles go out of scope).
**Fix:** destroy finished entities; check for `kNullEntity` where unbounded
spawning is possible. Logs an error when exhausted.

### 35. Scene double-moves collider entities

**Symptom:** entities with `Velocity + Collider` move twice as fast (or
ignore damping).
**Cause:** `Scene::update` integrates velocity **only** for entities without
a `Collider`; collider entities are moved by `PhysicsWorld` (which ignores
`Velocity::accel/damping/maxSpeed` — those live only in the ECS integrator).
**Fix:** don't mix the two models: either physics drives the entity (set
`body.vel`/`vel.y` for jumps; damping/accel are *not* applied), or you manage
movement yourself without a `Collider`. If you need damping with physics, add
it to the body manually each frame.

### 36. Static collider transforms

**Symptom:** editing `body->pos` of a static does nothing next frame; moving
the Transform *does* work.
**Cause:** `syncPhysics` copies Transform → body every update; writeback
skips statics.
**Fix:** move/tween statics via `Transform::pos` (it re-syncs), or use a
kinematic body for script-moved geometry.

## Audio and assets

### 37. No sound (and it's fine)

**Symptom:** `_audio().valid()` false, all plays return 0.
**Cause:** no audio device / `initAudio = false` — init soft-fails so games
run muted.
**Fix:** never gate gameplay on audio; optionally show a "muted" indicator.
Handle invalid `SoundHandle` from `loadSound`.

### 38. A sound stops unexpectedly

**Symptom:** long background loop cuts out.
**Cause:** the 32-voice pool steals the oldest **non-music** voice; music
voices are protected but only for `playMusic`.
**Fix:** use `playMusic` for loops; limit simultaneous one-shots (rate-limit
variation sounds).

### 39. `loadMemory` "failed to decode"

**Symptom:** synthesized audio works in the example but not your code.
**Cause:** `loadMemory` expects **encoded file bytes** (a WAV header + PCM,
MP3, OGG, FLAC), not raw PCM samples.
**Fix:** build the 44-byte WAV header (see 08-audio-assets) — or ship a file
and use `loadFile`.

### 40. Asset pointer invalidation

**Symptom:** dangling `Texture2D*` after `clear()`/`unloadTexture`.
**Cause:** cached pointers are stable only until those calls.
**Fix:** don't clear while anything holds pointers; unload explicitly when
sure. Hot reload itself is safe (mutates in place).

## Numbers and sanity

### 41. `frameMs()` is 0 and zones are empty

**Symptom:** profiler always zero.
**Cause:** `FORGE_PROFILING=0` build, or zones used outside a frame begun by
the engine.
**Fix:** check the CMake option; use `FORGE_ZONE` only inside
`run*`/`beginFrame` frames.

### 42. Pixel colors differ between runs of a "deterministic" capture

**Symptom:** shot hashes differ; CI screenshot compare flakes.
**Cause:** non-determinism sources: `sharedRandom()`, wall-clock time
(`Time::elapsed`, `fps` driving visuals), `std::rand`, unordered iteration
feeding behavior, vsync/timing-dependent logic, hot reload.
**Fix:** fixed `dt` only, seeded `Random` members, no wall clock in
simulation or visuals, and run the same frame count. Both example games are
built this way — copy their structure.

### 43. ASan reports leaks at exit

**Symptom:** leak reports from debug runs.
**Cause:** `Texture2D::white()` and (in some drivers) GL internals are
deliberately never destroyed — deleting GL objects after context teardown is
worse than leaking a 1×1 texture.
**Fix:** don't "fix" the singleton; if you must run leak checks, suppress the
known one. Engine-owned GL objects are freed by `Engine`/`Framebuffer`
destructors while the context is alive.
