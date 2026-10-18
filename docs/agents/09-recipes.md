# 09 — Recipes

Task-oriented patterns. Everything here compiles against the public headers;
adapt variable names to your game. For deeper explanations follow the links to
the module pages.

## Window, title, quitting

```cpp
forge::EngineConfig cfg;          // window fields are ignored in 0.1.0
cfg.quitOnEscape = false;         // set true if ESC should quit globally
forge::Engine engine(cfg);
if (!engine.valid()) return 1;
engine.window().setTitle("My Game");   // title must be set at runtime

// in the frame callback:
if (engine.input().pressed(forge::Key::P)) engine.quit();
```

Closing the window (or `engine.quit()`) makes the loops exit; in a manual
`beginFrame` loop it returns `false`.

## World pass + HUD pass

The standard frame: one batch pass through the camera, one through the default
screen projection. Submit world first, HUD last.

```cpp
forge::IVec2 vp = engine.viewport();
engine.clear(0.07f, 0.07f, 0.10f);

auto& batch = engine.batch();
batch.begin(cam.viewProj(vp.x, vp.y));
scene.draw(batch);                          // world-space things
batch.end();

batch.begin(engine.defaultProj());          // screen space, Y-up, (0,0) at center
font.draw(batch, "SCORE 1200", {-vp.x/2.0f + 16, vp.y/2.0f - 16}, 0.75f, forge::Color::white());
batch.end();
```

Drawn text may overlap between passes; text always wins if submitted later.

## Letterboxed virtual resolution (pixel-art)

Fixed internal resolution scaled to the window with black bars; include mouse
translation if you use the mouse.

```cpp
const float VW = 320, VH = 180;

void render(forge::Engine& engine, /* game, camera, font… */) {
    forge::IVec2 d = engine.viewport();
    float scale = std::min(d.x / VW, d.y / VH);
    int vpW = (int)(VW * scale), vpH = (int)(VH * scale);
    int vx = (d.x - vpW) / 2, vy = (d.y - vpH) / 2;

    engine.clear(0, 0, 0);                       // bars
    forge::gl::glViewport(vx, vy, vpW, vpH);     // letterbox viewport

    auto& batch = engine.batch();
    batch.begin(forge::Mat4::ortho(0, VW, 0, VH));   // virtual pixels, Y-up
    // draw in 0..VW × 0..VH coordinates
    batch.end();

    forge::gl::glViewport(0, 0, d.x, d.y);       // restore for the next pass
}
```

Map the mouse back into virtual pixels when needed:
`vx_mouse = (in.mousePos().x - vx) / scale`, then `world = {vx_mouse, VH - vy_mouse}`
for the Y-up ortho above.

## Pause that still renders

Pausing is a game-state decision, not an engine feature: skip your update,
keep drawing (so the pause menu is visible). Input keeps being polled, so you
can unpause. Works the same in both loop styles.

```cpp
bool paused = false;
engine.runFixed(
    [&](float dt) {
        auto& in = engine.input();
        if (in.pressed(forge::Key::Escape) || in.pressed(forge::Key::P)) paused = !paused;
        if (paused) return;                     // simulation frozen
        scene.update(dt);
    },
    [&]() { render(engine, paused); });         // render always draws
```

Note: while paused, `runFixed` still feeds real time to its internal stepper
and still calls `update` (which returns immediately), so the accumulator keeps
draining — when you unpause there is no banked backlog beyond the normal
0..`maxSteps` behavior.

## Camera follow, shake, and bounds

```cpp
forge::Camera2D cam;
cam.zoom = 2.0f;
cam.setMaxShake(28.0f);

// per frame:
cam.follow(player.pos + lookAhead, 8.0f, dt);   // exponential smoothing
cam.position.x = forge::clamp(cam.position.x, level.minX + halfW, level.maxX - halfW);
cam.position.y = forge::clamp(cam.position.y, level.minY + halfH, level.maxY - halfH);
cam.update(dt);                                  // decay trauma; MUST run every frame

// events:
cam.addTrauma(0.35f);                            // hit / explode
```

`follow` has no deadzone; for one, only call `follow` when the player is
outside a rectangle around `cam.position`.

## Input: bindings and gamepads

```cpp
// once, after constructing the engine:
auto& in = engine.input();
in.bindKey("left", forge::Key::A);   in.bindKey("left", forge::Key::Left);
in.bindKey("right", forge::Key::D);  in.bindKey("right", forge::Key::Right);
in.bindPad("left", forge::GamepadButton::DpadLeft);
in.bindPad("right", forge::GamepadButton::DpadRight);
in.bindKey("restart", forge::Key::R);
in.bindPad("restart", forge::GamepadButton::Start);

// per frame:
float move = (in.actionDown("right") ? 1.0f : 0.0f) - (in.actionDown("left") ? 1.0f : 0.0f);
move += in.moveAxis(0).x;               // sticks (pad 0 only; -1 = all pads)
move = forge::clamp(move, -1.0f, 1.0f);
if (in.actionPressed("restart")) restart();

// hotplug feedback:
if (in.padCount() > 0 && in.padPressed(0, forge::GamepadButton::A)) confirm();
```

Single-player default: `moveAxis()` (no argument) already merges WASD +
arrows + every connected pad.

## Mouse picking

```cpp
forge::IVec2 vp = engine.viewport();
forge::Vec2 world = cam.screenToWorld(in.mousePos(), vp.x, vp.y);
if (in.mousePressed(forge::MouseButton::Left) && card.contains(world))
    flip(card);
```

Remember `Rect::contains` is inclusive and `world` here is Y-up world space.

## Timers without a framework

The engine has no timer system — fixed-step games should use plain members
decremented by `dt`:

```cpp
float invuln = 0.0f;                       // seconds remaining
// update(dt):
invuln = std::max(0.0f, invuln - dt);
if (hit && invuln <= 0) { invuln = 1.0f; takeDamage(); }
```

For variable-step loops apply the same idea — `dt` is the clamped frame
delta. Do not use wall-clock calls inside simulation code (kills
determinism/headless capture).

## Grid games without physics (Snake/Breakout style)

Tile/grid games don't need the physics world: keep a 2D state array and move
on a fixed cadence. This is exactly how `08_snake` works (30×20 cells, 20 px).

```cpp
constexpr int GW = 30, GH = 20, CELL = 20;
std::vector<int> cells(GW * GH, 0);        // 0 empty, 1 snake, 2 food
float stepTimer = 0.0f, stepTime = 0.155f;

// update(dt):
stepTimer += dt;
while (stepTimer >= stepTime) {
    stepTimer -= stepTime;
    advanceGrid();                          // discrete movement here
    stepTime = std::max(0.072f, stepTime * 0.995f);  // speed ramp, floored
}

// render: interpolate between the previous and current grid position using
// stepTimer / stepTime for smooth sub-cell motion.
forge::Vec2 cellCenter(int x, int y) {
    return {CELL * 0.5f + x * CELL, CELL * 0.5f + y * CELL};
}
```

Death condition = next cell out of bounds or already occupied; food = random
empty cell from a deterministic `forge::Random` member.

## Physics platformer piece

```cpp
forge::Body* p = world.get(playerId);
float ax = in.moveAxis(0).x;
p->vel.x = ax * 320.0f;
if ((in.pressed(forge::Key::Space) || in.padPressed(0, forge::GamepadButton::A)) && p->onGround)
    p->vel.y = 780.0f;

// variable jump height: release early -> cut the rise
if (!in.isDown(forge::Key::Space) && !in.isDown(forge::Key::W) && p->vel.y > 0)
    p->vel.y *= 0.55f;

// coyote time + jump buffer (classic feel polish):
if (p->onGround) coyote = 0.10f; else coyote = std::max(0.0f, coyote - dt);
if (jumpPressed) buffer = 0.12f; else buffer = std::max(0.0f, buffer - dt);
if (buffer > 0 && coyote > 0) { p->vel.y = 780.0f; buffer = coyote = 0; }

// respawn:
if (p->pos.y < -200) { p->pos = spawn; p->vel = {0, 0}; }
```

Everything above runs in `runFixed`'s update at `world.step(dt)` cadence.

## Tilemap: load, solids, collision

`02_platformer` builds its map procedurally; `Tilemap::loadTiledJson` loads a
real Tiled export.

```cpp
forge::Tilemap map;
if (!map.loadTiledJson("assets/level1.json")) return 1;
forge::Texture2D* sheet = engine.assets().loadTexture(map.tilesetPath());
// columns = tiles per row in the sheet image; tileCount = total tiles
map.setTileset(sheet, 8, 64);

for (int id = 1; id <= 64; ++id)
    if (isWallId(id)) map.setSolid(id);

// validate collision boxes after hand-editing tiles:
for (auto& r : map.solidsInRect(area)) { /* … */ }

// per frame, resolve the player box:
forge::Rect box = forge::Rect::fromCenter(pos, {22, 28});
forge::Vec2 fix;
if (map.collideRect(box, &fix)) { pos += fix; box.x += fix.x; box.y += fix.y; }
```

Render inside the camera pass:

```cpp
forge::Rect view = forge::Rect::fromCenter(cam.position, {vp.x / cam.zoom, vp.y / cam.zoom});
map.draw(batch, {0, 0}, view);        // origin = world pos of tile (0,0)
```

## One-shot particles

```cpp
// setup once
forge::EmitterConfig boom;
boom.maxParticles = 512; boom.looping = false; boom.spawnRate = 0;
boom.life = 0.6f; boom.lifeVar = 0.2f;
boom.size = 10.0f; boom.sizeVar = 5.0f; boom.endSize = 0.0f;
boom.start = forge::Color::fromHex(0xFFD166FF);
boom.end   = forge::Color::clear();
boom.vel = {0, 120}; boom.velVar = {260, 200};
boom.damping = 3.0f;
forge::ParticleSystem sparks(boom);

// at the event:
sparks.setPosition(hitPos); sparks.emit(24);

// per frame, before batch.end() of the world pass:
sparks.update(dt);
sparks.draw(batch);
```

Add variation with `boom.seed` different per system (or re-`configure` with a
new seed) — the same seed always produces the same burst (good for tests).

## Sprite animation

```cpp
// once
forge::Texture2D* sheet = engine.assets().loadTexture("assets/hero.png");
forge::Animator hero;
hero.addClip(forge::AnimClip::gridRow("idle", 0, 4, 32, 32, 8.0f));   // row 0
hero.addClip(forge::AnimClip::gridRow("run", 1, 6, 32, 32, 12.0f));    // row 1
hero.play("idle");

// state switching (cheap to call every frame):
hero.play(std::abs(vel.x) > 1 ? "run" : "idle");
hero.setSpeed(std::abs(vel.x) > 1 ? std::abs(vel.x) / 120.0f : 1.0f);

// per frame:
hero.update(dt);
hero.draw(batch, *sheet, pos, {48, 48}, 0.0f, forge::Color::white(), vel.x < 0);
```

The grid row indexing counts from the **top of the sheet image** regardless
of the flipY convention — the conversion happens inside `Animator::draw`.

## Procedural textures

Zero-asset placeholders and procedural tilesets:

```cpp
forge::Texture2D checker;
checker.createChecker(8, forge::Color::fromHex(0x2D2A32FF), forge::Color::fromHex(0x3A3642FF));

// Raw pixel upload: rows must be BOTTOM-UP to match the engine's flipY convention
const int W = 128, H = 32;
std::vector<uint8_t> px(W * H * 4);
for (int y = 0; y < H; ++y)                // paint in image space (top-down)
    for (int x = 0; x < W; ++x) { /* … */ }
std::vector<uint8_t> flipped(px.size());   // flip rows vertically
for (int y = 0; y < H; ++y)
    std::memcpy(&flipped[y * W * 4], &px[(H - 1 - y) * W * 4], W * 4);
forge::Texture2D tileset;
tileset.loadMemory(flipped.data(), W, H, 4, forge::TextureFilter::Nearest);
```

(If you skip the flip, the texture is upside down relative to the pixel-rect
helpers.)

## Sound effects with no audio files

Use the `synthWav` pattern from [08-audio-assets.md](08-audio-assets.md) or
copy `examples/08_snake.cpp` lines ~96-142. Generate all SFX once at startup:

```cpp
auto hitWav  = synthWav({{440, 220, 0.09f, 0.6f, 1, 0.1f}});
auto coinWav = synthWav({{988, 1319, 0.07f, 0.5f, 0, 0.0f}});
forge::SoundHandle sHit  = engine.audio().loadMemory(hitWav.data(), hitWav.size());
forge::SoundHandle sCoin = engine.audio().loadMemory(coinWav.data(), coinWav.size());

// on event (pitch jitter makes repeats less grating):
engine.audio().play(sHit, 0.8f, 0.0f, 0.92f + 0.16f * rng.nextFloat());
```

## Deterministic headless capture + CI

The pattern from `07_breakout`/`08_snake`: run N fixed steps (no wall clock),
render one frame into an offscreen `Framebuffer`, read it back, write a PNG,
and exit non-zero if an invariant ("sane") check fails. Same args → same
pixels, so it doubles as a screenshot test.

```cpp
#include "stb_image_write.h"     // vendored; third_party is on the include path

bool saveScreenshot(const std::string& path, forge::IVec2 vp) {
    std::vector<uint8_t> px((size_t)vp.x * vp.y * 4);
    forge::gl::glReadPixels(0, 0, vp.x, vp.y, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    std::vector<uint8_t> flipped(px.size());           // GL bottom-up -> PNG top-down
    const size_t row = (size_t)vp.x * 4;
    for (int y = 0; y < vp.y; ++y)
        std::memcpy(&flipped[(size_t)y * row], &px[(size_t)(vp.y - 1 - y) * row], row);
    return stbi_write_png(path.c_str(), vp.x, vp.y, 4, flipped.data(), (int)row) != 0;
}

int main(int argc, char** argv) {
    // ... parse Args (--shot path, --frames N, --seed, ...)
    forge::EngineConfig cfg;
    cfg.window.vsync = shotPath.empty();               // no vsync for capture
    forge::Engine engine(cfg);
    if (!engine.valid()) return 1;

    Game game; game.setSeed(seed); game.init(engine);

    // deterministic simulation: fixed dt, no wall clock
    for (int i = 0; i < frames; ++i) game.update(1.0f / 60.0f);

    forge::Framebuffer fbo;
    const bool offscreen = fbo.create(1024, 768);
    game.renderTo(offscreen ? &fbo : nullptr);         // renders into the FBO or screen
    const forge::IVec2 shotSize = offscreen ? forge::IVec2{1024, 768} : engine.viewport();
    if (offscreen) fbo.bind();                         // read back the target
    const bool ok = saveScreenshot(path, shotSize);
    if (offscreen) forge::Framebuffer::unbind(engine.viewport().x, engine.viewport().y);

    const bool sane = game.healthy();                  // no NaN, bounds, invariants
    std::printf("shot: %s sane=%d %s\n", path.c_str(), sane, ok ? "OK" : "FAILED");
    return ok && sane ? 0 : 1;
}
```

The `renderTo(fb)` helper in the examples is:

```cpp
void renderTo(forge::Framebuffer* fb) {
    if (fb) fb->bind();
    engine->clear(0, 0, 0);
    // ... camera pass + HUD pass, batch.begin/end ...
    if (fb) forge::Framebuffer::unbind(engine->viewport().x, engine->viewport().y);
}
```

For interactive `--demo` mode, run a normal loop instead of the fixed loop.
For CI, either capture or just run `frames` updates and exit with the sane
flag (`CI=1 ./build/07_breakout --frames 240`). Seed everything: gameplay RNG,
particle systems, camera shake is already deterministic.

## Profiler overlay

```cpp
// FORGE_PROFILING comes from CMake; don't define the macro by hand.
// Per frame, in the HUD pass:
FORGE_ZONE("hud");
char buf[96];
std::snprintf(buf, sizeof(buf), "%.1f ms  %.0f fps  %d draws",
              forge::Profiler::instance().frameMs(), engine.fps(), engine.drawCalls());
font.draw(batch, buf, {vp.x/2.0f - 230, vp.y/2.0f - 24}, 0.65f, forge::Color::fromHex(0x90BE6DFF));
```

Zone timings are available via `zoneMs("name")`; frame history (120 frames)
via `history()` for a graph.

## ECS: safe structural changes

```cpp
// collect first, mutate after — never inside view():
std::vector<forge::Entity> dead, spawned;
scene.registry().view<forge::Health>([&](forge::Entity e, forge::Health& h) {
    h.hp -= 1;
    if (h.hp <= 0) dead.push_back(e);
    if (h.hp == 7) spawned.push_back(e);   // example deferred spawn
});
for (auto e : dead)    scene.destroy(e);
for (auto e : spawned) { auto n = scene.create("child"); /* … */ }
```

`Scene::update`'s script pass already snapshots entities, so `onUpdate`
scripts *may* spawn/destroy (it re-checks aliveness between callbacks).

## Saving a high score (no save system in the engine)

Plain `FILE*`/`std::fstream` is the intended approach; there is no serialization
API. Keep it small:

```cpp
int loadBest(const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return 0;
    int v = 0; std::fread(&v, sizeof(v), 1, f); std::fclose(f);
    return v > 0 ? v : 0;
}
void saveBest(const char* path, int v) {
    if (std::FILE* f = std::fopen(path, "wb")) { std::fwrite(&v, sizeof(v), 1, f); std::fclose(f); }
}
```

For anything structured, serialize fixed-size fields explicitly (platform
endianness/ABI match is acceptable for same-machine saves).

## Window resize-aware HUD

`engine.viewport()` changes when the user resizes. Rebuild layout from it each
frame instead of caching pixel positions:

```cpp
forge::IVec2 vp = engine.viewport();
float rightX = vp.x / 2.0f - 16;
font.draw(batch, "PAUSED", {rightX - font.widthOf("PAUSED", 1.0f), 0}, 1.0f, forge::Color::white());
```

For games with fixed-layout HUDs, prefer the letterbox/scale recipe above so
layout coordinates stay constant.
