# 06 — Physics

Header: `forge/physics/physics.hpp`. Implementation: `src/physics/physics.cpp`.

An **arcade AABB solver**, not a rigid-body engine. Read the box below before
choosing it.

> **Solver scope:** axis-aligned boxes only; unit masses; no rotation/torque;
> one relaxation pass per substep (deep stacks can jitter); discrete collision
> with auto-substeps (fast small bodies can still tunnel); linear friction
> skid model. Tuned for platformers and top-down 2D.

## Body

```cpp
enum class BodyType { Static, Kinematic, Dynamic };

struct Body {
    uint32_t id = 0;              // assigned by addBody()
    BodyType type = BodyType::Dynamic;
    Vec2 pos{0,0};                // CENTER (not corner)
    Vec2 vel{0,0};
    Vec2 size{32,32};             // full extents
    float gravityScale = 1.0f;
    float friction = 0.0f;        // ground friction applied while onGround
    float restitution = 0.0f;     // bounciness (pair uses the max)
    bool onGround = false;        // set by the solver during step()
    bool sensor = false;          // reports overlap, never resolves
    void* userData = nullptr;

    Rect aabb() const;            // Rect::fromCenter(pos, size), Y-up
};
```

| Type | Gravity | Moved by | Collides with static | Collides with dynamic | Pushed by others |
|---|---|---|---|---|---|
| `Static` | no | you (pos edits) | — | yes (pushes dynamics) | never |
| `Kinematic` | no | `vel` at step | pushed out (finite mass) | pushes (infinite mass) | only by static/kinematic |
| `Dynamic` | yes ×`gravityScale` | gravity, `vel`, contacts | stops/pushes out | both resolve | yes |

The kinematic rule is subtle and deliberate: kinematic-vs-dynamic resolves as
if the kinematic body were *infinitely heavy* (it pushes dynamics, is not
pushed back), while kinematic-vs-static behaves as finite mass so script-moved
platforms get pushed out instead of tunneling through walls.

## World

```cpp
class PhysicsWorld {
    void setGravity(Vec2 g);          // default (0, -980)
    Vec2 gravity() const;
    void setCellSize(float s);        // broadphase hash cell, default 64

    uint32_t addBody(const Body& b);  // returns id
    void removeBody(uint32_t id);     // O(1) swap-erase
    void clear();
    Body* get(uint32_t id);           // nullptr if absent; ⚠ see pointer rules
    const Body* get(uint32_t id) const;
    size_t bodyCount() const;

    void onContact(ContactCallback cb);  // std::function<void(const Contact&)>
    void step(float dt);

    std::vector<uint32_t> queryRect(Rect r) const;
    bool raycast(Vec2 from, Vec2 to, uint32_t* hitId = nullptr,
                 Vec2* hitPos = nullptr) const;
};
```

### Pointer rules

`get()` returns a pointer **into the internal vector**. `addBody`/`removeBody`
swap-erase and reallocate: any previously fetched pointer is invalid
afterwards. Re-fetch after structural changes. (The examples hold
`playerId` and call `world.get(playerId)` per frame — the safe pattern.)

Non-const `get()` also marks the query grid dirty; the const overload does not.

## Contact

```cpp
struct Contact {
    uint32_t a = 0, b = 0;   // body ids (a.id/b.id)
    Vec2 normal{0,0};        // unit axis; points such that moving `a` along it (or `b` against it) separates the pair
    float penetration = 0;   // overlap depth along normal
};
using ContactCallback = std::function<void(const Contact&)>;
```

`normal` is the minimal translation axis from the AABB overlap (always one of
±X/±Y). Practical ground checks: `|normal.y| > 0.7` and testing which side
your player id is on (`c.a == playerId` vs `c.b == playerId`) tells you the
push direction.

### When the callback fires

- For every overlapping pair detected in a substep — **sensors included**, and
  even static-vs-static pairs if at least one is a sensor.
- Multiple times per frame when a pair stays in contact across substeps.
- During `step()`, before resolution for non-sensors.

So the callback is a *stream of events*, not a stable "set of contacts". If
your gameplay wants "landed once", compare against a stored `onGround` from
last frame or track previous overlap sets yourself.

```cpp
world.onContact([&](const forge::Contact& c) {
    if ((c.a == coinId || c.b == coinId) && !c.sensorIgnoredByYou)
        collectCoin();
});
```

## step(dt) — exact behavior

1. `dt <= 0` → no-op. `dt > 0.1` → clamped to 0.1 s (no tunneling past this
   budget).
2. Substeps: `sub = clamp(ceil(dt / (1/120)), 1, 8)`, `h = dt / sub`.
3. Per substep, per body, in id order: clear `onGround`; if Dynamic add
   `gravity * gravityScale * h` to `vel`; move `pos += vel * h`. Kinematic
   bodies move by `vel` but get no gravity.
4. Broadphase: spatial hash over cells (`cellSize`), deduped AABB pairs.
5. Solve each pair once per substep:
   - compute MTV (min penetration axis),
   - fire `onContact` for every overlap,
   - sensors stop here (no resolution),
   - positional correction split by inverse mass (`Static` = 0, `Dynamic` = 1,
     `Kinematic` = 1 only vs static/kinematic),
   - velocity: remove inbound relative velocity, add restitution scaled by
     `max(a.restitution, b.restitution)`,
   - if the separating axis is up (`normal.y > 0.7` and `a` dynamic) mark
     `a.onGround` and apply `friction` to `a.vel.x` with
     `1 - clamp01(friction * h * 60)`; symmetrically for the other side.

Because it is one pass, overlapping/stacked dynamics interpenetrate slightly
and can jitter. If you need crisp stacking, handle it at the game level
(snap positions, mark sleepers, use tiles instead).

## Queries

### `queryRect(Rect r)` → body ids

AABB-overlap test against the cached spatial grid; **not** a linear scan. The
grid is rebuilt lazily when marked dirty (after `step`, structural changes, or
a non-const `get`). Result order is unspecified.

```cpp
for (uint32_t id : world.queryRect(blastRect)) {
    if (forge::Body* b = world.get(id)) { b->vel += pushDir * 300.0f; }
}
```

### `raycast(from, to, hitId, hitPos)`

Nearest **non-sensor** body entry along the segment, slab test per body.

- Bodies that **contain the ray origin are skipped** (otherwise everything
  enclosing the origin would hit at t = 0, hiding the true surface).
- `hitPos = from + (to - from) * tEntry`; returns false when nothing is hit.
- O(bodies) — fine for hundreds; if you need thousands, query a rect first.

```cpp
forge::Vec2 hit;
uint32_t id = 0;
if (world.raycast(gunMuzzle, aimPoint, &id, &hit))
    spawnImpact(hit);
```

## Determinism

`step()` is allocation-free after warm-up and iterates bodies/pairs in stable
order (ids, sorted pair indices). Same build + same inputs + same dt sequence
= same simulation, which is what the headless shot tests rely on. Avoid
`std::unordered_map` iteration driving physics-visible behavior, and never
insert/remove bodies from inside the contact callback (that's a structural
change mid-step).

## Choosing move modes

- **Platformer**: gravity `(0, -1800…-2400)`, player Dynamic with
  `friction ≈ 0.2`, jump sets `vel.y` when `onGround`. See
  `02_platformer.cpp`.
- **Top-down**: `world.setGravity({0,0})`, drive `vel` directly, add
  `damping` on the body or clamp speed yourself; use sensors for triggers.
- **Moving platforms**: `Kinematic`, set `vel` (or edit `pos` before step and
  let the next step correct mutual overlaps); riders get pushed.
- **Only need overlaps / grid collision**: skip the physics world entirely and
  use `Rect::overlaps`, `Tilemap::collideRect`, or tile arrays (what 07/08 do).

## Scene integration (cross-ref)

`Scene` owns a `PhysicsWorld` and syncs `Transform + Collider` entities into
it every update — see [07-scene-ecs.md](07-scene-ecs.md) for the dynamic/
static/kinematic mapping, the default gravity trap, and writeback rules.
