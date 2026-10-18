# 05 — Animation, Tweens, Particles

Headers: `forge/graphics/animation.hpp`, `forge/graphics/particles.hpp`.

## Spritesheet animation — `AnimClip` + `Animator`

A clip is a list of pixel rects in one atlas texture plus fps + loop flag.

```cpp
struct AnimClip {
    std::string name;
    std::vector<Rect> frames;   // pixel rects in the atlas
    float fps = 12.0f;
    bool loop = true;

    static AnimClip gridRow(name, row, cols, frameW, frameH, fps = 12, loop = true);
    static AnimClip grid(name, cols, rows, frameW, frameH, fps = 12, loop = true);
    float duration() const;     // frames / fps
};
```

Convenience builders assume a grid layout: `gridRow` takes row `row` (0-based
from the left-top of the sheet — rects are y*rowH from the top of the image,
matching how `draw` flips UVs), `grid` lists all frames row-major.

```cpp
class Animator {
    void addClip(const AnimClip& clip);        // add all clips BEFORE play()
    bool play(const std::string& name, bool restart = false);
    void stop();  void pause();  void resume();
    void update(float dt);
    const AnimClip* current() const;
    int  frameIndex() const;  Rect currentFrame() const;
    bool finished() const;                     // non-looping clip reached the end
    bool playing() const;                      // playing && !paused
    void setSpeed(float s);
    void setFrameEvent(const std::string& clip, int frame, std::function<void()> cb);
    void draw(SpriteBatch&, const Texture2D& tex, Vec2 pos, Vec2 size,
              float rot = 0, Color c = Color::white(), bool flipX = false) const;
};
```

Notes:

- `play(name)` is a no-op restart-wise: playing the *same* clip again without
  `restart = true` just resumes it. `play` on a different clip resets to frame 0.
- Frame events fire when a frame is first displayed (crossing into it),
  keyed per clip + frame index.
- `update` clamps to the last frame for non-looping clips; check `finished()`
  to transition.
- `draw` submits a `SpriteDraw` with the frame's UV rect computed for you;
  `pos` is the center and size the target display size.

```cpp
forge::AnimClip walk = forge::AnimClip::gridRow("walk", 0, 6, 32, 32, 12.0f);
forge::AnimClip idle = forge::AnimClip::gridRow("idle", 1, 4, 32, 32, 6.0f);
forge::Animator anim;
anim.addClip(walk);          // before play()
anim.addClip(idle);
anim.play("walk");
anim.setSpeed(1.5f);
anim.setFrameEvent("walk", 2, [&]{ dust.emit(4); });   // footstep puff

// per frame
anim.update(dt);
anim.draw(batch, *heroTex, heroPos, {48, 48});
```

Rehash safety: `clips_` is an `unordered_map`; adding clips invalidates a
cached internal pointer, which `current()` re-resolves by name each call — so
add everything up front and never call `addClip` mid-play.

## Tweens — fire-and-forget interpolation

```cpp
enum class TweenMode { Once, Loop, PingPong };

struct Tween {
    float from = 0, to = 1, duration = 1, elapsed = 0;
    TweenMode mode = TweenMode::Once;
    float (*ease)(float) = easing::outCubic;  // any forge::easing::* function
    std::function<void(float)> onUpdate;      // called with the eased value
    std::function<void()> onDone;             // Once only
    bool done = false;

    void update(float dt);  float value() const;  void reset();
};

class Tweens {
    int  add(const Tween& t);   // returns the item index (not a durable handle)
    void update(float dt);
    void clear();  bool empty() const;
};
```

Semantics (implemented in `animation.cpp`):

- `onUpdate(value())` is called every `update` with the **eased, interpolated
  value** — not the raw t.
- `Once`: when `t >= 1`, calls `onUpdate(ease(1))`, then `onDone`, sets
  `done`, and `Tweens::update` swap-removes it (so iteration order/indices
  shift; don't stash the returned id).
- `Loop`: on completion it keeps the overflow (`fmod`), so long frames don't
  lose time.
- `PingPong`: on completion it swaps `from`/`to` and carries overflow; a very
  long frame can cross multiple legs in one update.

```cpp
forge::Tweens fx;
fx.add({.from = 1.0f, .to = 1.6f, .duration = 0.18f, .mode = forge::TweenMode::PingPong,
        .ease = forge::easing::outQuad,
        .onUpdate = [&](float s){ pop.scale = s; }});

// per frame (fixed step is fine):
fx.update(dt);
```

Typical uses: scale pops, fade in/out (drive `SpriteDraw.color` alpha), score
counters (lerp integer display value in `onUpdate`), UI slides. For one-shot
camera shakes use `Camera2D::addTrauma` instead.

## Particles — `ParticleSystem` + `EmitterConfig`

CPU pool, zero allocations after `configure`, swap-removal, deterministic
(`seed`). Particles are drawn through the sprite batch using one texture (or
a white square).

```cpp
struct EmitterConfig {
    const Texture2D* texture = nullptr; // null = white square
    int   maxParticles = 1024;          // pool size, fixed at configure()
    float spawnRate = 100.0f;           // particles/sec when looping
    bool  looping = true;

    Vec2 posVar{0,0};      // spawn box: origin ± posVar (each axis independent)
    Vec2 vel{0,100};       // base velocity
    Vec2 velVar{40,40};    // ± random spread on velocity
    Vec2 accel{0,0};       // constant acceleration (gravity, wind)
    float life = 1.0f, lifeVar = 0.3f;  // ± random, clamped to 0.01
    float size = 8.0f, sizeVar = 3.0f, endSize = 0.0f; // size lerps → endSize over life
    Color start = white, end = white;   // color lerps start → end over life
    float damping = 0.0f;  // exponential drag (never negative/exploding)
    float rotVel = 0, rotVelVar = 0;
    uint64_t seed = 12345;
};

class ParticleSystem {
    ParticleSystem();  explicit ParticleSystem(const EmitterConfig&);
    void configure(const EmitterConfig&);   // resizes pool, reseeds rng
    void setPosition(Vec2 p);  Vec2 position() const;

    void emit(int count);      // burst
    void emitOne();
    void update(float dt);     // simulate (+ auto-spawn if looping && emitting)
    void draw(SpriteBatch&) const;

    void clear();  int alive() const;  int capacity() const;
    void setEmitting(bool e);  bool emitting() const;
};
```

Semantics:

- Auto-spawn happens in `update` only when `emitting == true && looping ==
  true && spawnRate > 0`. A **burst** emitter sets `looping = false` (or
  `spawnRate = 0`) and calls `emit(n)` whenever something happens; `update`
  still simulates existing particles. `setEmitting(false)` silences a looping
  emitter without stopping the ones already alive.
- The pool never exceeds `maxParticles`; emitters over budget silently drop
  (no allocations, no asserts).
- Over lifetime `t` (0 fresh → 1 dead): size = `lerp(size, endSize, t)` and
  color = `start.lerped(end, t)`. Set `end = Color::clear()` for fade-outs and
  `endSize = 0` for shrink-outs.
- `update` is simulation — call it at your fixed step (or variable dt) before
  `draw`. `draw` only submits quads; it does not update.

Worked example — a landing dust puff:

```cpp
forge::EmitterConfig dust;
dust.maxParticles = 256;
dust.looping = false;         // burst-only
dust.spawnRate = 0;
dust.life = 0.45f; dust.lifeVar = 0.15f;
dust.size = 6.0f;  dust.sizeVar = 3.0f; dust.endSize = 0.5f;
dust.start = forge::Color::fromHex(0xF1FAEEAA);
dust.end   = forge::Color::clear();
dust.vel = {0, 80}; dust.velVar = {120, 40};
dust.damping = 2.0f;

forge::ParticleSystem particles(dust);

// on landing:
particles.setPosition(pos + forge::Vec2{0, -12});
particles.emit(14);

// per frame:
particles.update(dt);
particles.draw(batch);
```

Continuous emitter (torch, thruster, smoke stack): leave `looping = true`,
set `spawnRate`, keep `setPosition(...)` following the source, and
`setEmitting(false)` when off. Because the emitter spawns around a single
point per frame, fast-moving emitters trail particles in discrete clumps —
for very fast motion, spawn at several interpolated positions or crank the
rate.
