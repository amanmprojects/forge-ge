# 07 — Scene and ECS

Headers: `forge/scene/ecs.hpp`, `components.hpp`, `scene.hpp`.

## Entity model

Entities are **generational handles packed into 32 bits**: 20-bit index +
12-bit generation (`makeEntity`, `entityIndex`, `entityGen`). Index 0 is
reserved as `kNullEntity` (never issued). Because generation is part of the
handle, a stale reference to a destroyed entity reports `alive() == false`
instead of aliasing a recycled slot.

```cpp
using Entity = uint32_t;
inline constexpr Entity kNullEntity = 0;
inline constexpr uint32_t kMaxEntityIndex = 0x000FFFFFu; // 1,048,575 slots
inline constexpr uint32_t kMaxEntityGen   = 0xFFFu;

Entity  makeEntity(uint32_t idx, uint32_t gen);
uint32_t entityIndex(Entity e);
uint32_t entityGen(Entity e);
```

If the index space is exhausted, `Registry::create()` logs an error and
returns `kNullEntity` (it does not crash). Check for null when your game can
corrupt/spawn unboundedly.

## Registry

```cpp
class Registry {
    Entity create();                       // recycles freed indices; bumps generation on destroy
    void   destroy(Entity e);              // removes every component; stale handles become dead
    bool   alive(Entity e) const;
    void   clear();                        // wipes pools + all entities
    size_t entityCount() const;

    template <typename T, typename... Args> T& emplace(Entity, Args&&...);
    template <typename T> void remove(Entity);
    template <typename T> bool has(Entity) const;
    template <typename T> T& get(Entity);             // asserts has<T> — only when you KNOW
    template <typename T> const T& get(Entity) const;
    template <typename T> T* tryGet(Entity);          // nullptr instead of assert

    template <typename... Ts, typename Fn> void view(Fn&&) const;   // Fn(Entity, Ts&...)
};
```

Behavior details that matter:

- `emplace<T>` on a dead/null entity **asserts** (`assert(alive(e))`). In
  release builds with NDEBUG that check vanishes — keep lifetimes correct.
- `emplace` twice replaces the component's value (`T{args...}` assignment).
- `destroy` is generation-checked for idempotence: destroying a stale handle
  is a no-op (components can't be removed twice).
- `clear()` resets next-index; handles from before `clear()` are meaningless.

### `view<Ts>(fn)` — the iteration rules

```cpp
reg.view<forge::Transform, forge::Sprite>([&](forge::Entity e, forge::Transform& t, forge::Sprite& s) {
    // iterate every entity that has ALL listed components
});
```

1. Iterates the pool of the **first listed type**, skipping entities that lack
   the others. Put the *rarest/most selective* component first — it defines
   the scan cost. (The README's "smallest pool" phrasing describes the intent;
   the implementation is first-listed.)
2. Inside the callback you must **not** call `create`, `destroy`,
   `emplace`, or `remove` — structural changes invalidate the pool being
   iterated (dense vectors mutate under you). Collect work, then apply after
   the view returns. `Scene::update` and `Registry::destroy` already follow
   this pattern internally.
3. Callbacks receive real component references; writes persist.
4. A const overload exists for read-only passes.

Deferred structural changes pattern:

```cpp
std::vector<forge::Entity> dead;
reg.view<Health>([&](forge::Entity e, Health& h) { if (h.hp <= 0) dead.push_back(e); });
for (forge::Entity e : dead) reg.destroy(e);
```

## Built-in components

```cpp
struct Transform { Vec2 pos{0,0}; float rotation = 0; Vec2 scale{1,1}; };

struct Sprite {
    const Texture2D* texture = nullptr;
    Rect  src{0,0,0,0};      // PIXEL rect, top-left origin; empty = full texture
    Color color = Color::white();
    int   layer = 0;         // draw order (stable-sorted)
    bool  visible = true;
    bool  flipX = false, flipY = false;
    Vec2  size{0,0};         // 0 = texture size * Transform::scale
};

struct Velocity {
    Vec2  v{0,0};
    Vec2  accel{0,0};        // integrated by Scene for non-collider entities
    float damping = 0.0f;    // exponential: v *= exp(-damping*dt)
    float maxSpeed = 0.0f;   // clamp before integration; 0 = unlimited
};

struct Collider {
    Vec2  halfExtents{16,16}; // box around Transform.pos (full size = 2*halfExtents)
    bool  solid = true;       // false → sensor (overlap only)
    bool  sensor = false;     // report contacts, no pushback
    bool  kinematic = false;  // script-moved: pushes dynamics, not pushed by them
    float friction = 0.0f;    // ground friction while resting
    float gravityScale = 1.0f;
};

struct Tag { std::string name; };

// Data-only script id, saved with the scene. The game re-binds the behavior
// by name after load (a `Script` closure is code and is never serialized).
struct ScriptRef { std::string name; };

struct Script {
    std::function<void(Entity, Registry&, float)> onUpdate;
    std::function<void(Entity, Registry&)> onStart;
    bool started = false;     // managed by Scene
};
```

Note `Sprite` has no `Animator` integration — animators are your own
component/member; draw through `Animator::draw` in a custom pass, or set
`Sprite::src` from `anim.currentFrame()` each frame (Scene converts it for you).

## Scene

`Scene` is the convenience layer: a `Registry` + a `PhysicsWorld` + a fixed
update/draw pipeline.

```cpp
class Scene {
    Registry& registry();            const Registry& registry() const;
    PhysicsWorld& physics();         const PhysicsWorld& physics() const;

    Entity create(const std::string& tag = "");   // always adds Transform (+Tag if non-empty)
    void destroy(Entity e);                       // also removes its physics body
    void clear();

    void update(float dt);
    void draw(SpriteBatch& batch, const Camera2D* cam = nullptr);  // cam unused (see below)

    Entity findByTag(const std::string& tag);
    void each(const std::function<void(Entity)>& fn);              // entities with Transform
};
```

### `update(dt)` order (exact)

1. **Scripts**: snapshot all entities with `Script` first (so scripts may
   spawn/destroy safely), then for each: call `onStart(e, reg)` once, then
   `onUpdate(e, reg, dt)`; re-check `alive` between calls.
2. **Physics sync**: destroy/retract bodies whose entity died or lost
   Transform/Collider; then for every `Transform + Collider` entity
   create-or-update a body:
   - `kinematic = true` → `Kinematic`
   - else `Velocity` present → `Dynamic`
   - else → `Static`
   - `sensor = c.sensor || !c.solid`; `userData = (void*)(uintptr_t)entity`
     (recover the entity in contact callbacks); `body.pos = transform.pos`.
3. **Velocity integration** for entities that have `Transform + Velocity` but
   **no Collider**: apply `accel`, `damping`, `maxSpeed`, then `pos += vel*dt`.
   (Collider entities are moved by physics instead — never double-integrated.)
4. `physics().step(dt)`.
5. **Writeback**: for every non-static synced body, copy `body.pos` →
   `Transform::pos` and `body.vel` → `Velocity::v`. Static bodies never move,
   so their transforms are not overwritten.

The physics sync runs **every** update, so collider kinematics are limited:
`Scene` re-writes `body.pos = transform.pos` from your transform at the start
and the writeback copies back at the end — to teleport a collider entity, set
the transform; to drive a kinematic platform, set `Velocity` (or mutate
`scene.physics().get(id)` after the sync if you go direct).

⚠ **Default gravity trap:** `Scene`'s world starts at `(0, -980)` — a side-
scroller default. For a top-down game, do `scene.physics().setGravity({0,0})`
or every dynamic collider entity slides down.

### `draw(batch, cam)`

- Collects entities with `Transform + Sprite` where `visible`, **stable-sorts
  by `layer`** (equal layers keep insertion order), then submits draws.
- `Sprite::size` is used when both components are > 0; otherwise the size is
  `texture->size() * Transform::scale`. Textures render at their pixel size by
  default.
- `Sprite::src` (pixel, top-left) is converted to UVs for you.
- The `cam` parameter is currently unused — the batch's `viewProj` (set at
  `begin`) defines the view. Draw the scene inside a camera pass:

```cpp
batch.begin(cam.viewProj(vp.x, vp.y));
scene.draw(batch);            // world-space sprites
batch.end();
```

### Ownership of custom systems

`Scene` does not run arbitrary systems: write them as free functions per
frame (or as `Script` components). The 05 example mixes both — scripts for
per-entity behavior, then a manual pass for collision reactions.

```cpp
// per frame (before or after scene.update, your choice):
scene.registry().view<Bullet, Transform>([&](forge::Entity e, Bullet& b, forge::Transform& t) {
    t.pos += b.vel * dt;
    if (t.pos.x > 1280 || t.pos.x < -1280) toKill.push_back(e);
});
```

## When to use `Scene` vs raw `Registry`

- Use `Scene` when: entities map 1:1 to physics bodies (platformers, top-down
  shooters), you want start/update scripts, and layer-sorted rendering.
- Use a raw `Registry` (+ your own systems/members) when: you manage arrays of
  entities outside physics (bullet hells, particle-like swarms), need custom
  body shapes (Scene only makes boxes from `Collider`), or want full control
  of update order. `Registry` is standalone — you can use it with no
  `PhysicsWorld` at all.

Both live fine in the same game; `05_scene.cpp` shows a mid-size example, and
`02_platformer.cpp` shows physics without ECS.

## Serialization and prefabs

`forge/scene/serialization.hpp` saves the **data** components of a scene to
versioned JSON, and re-instantiates them as prefabs.

```cpp
#include <forge/forge.hpp>

// save / load a whole scene
forge::saveScene(scene, "save.json", &engine.assets());
forge::Scene loaded;
forge::SceneSaveStats stats;
if (forge::loadScene(loaded, "save.json", &engine.assets(), &stats))
    std::printf("%d entities, %d sprites\n", stats.entities, stats.sprites);

// prefabs
forge::Prefab hero = forge::makePrefab(scene, heroEntity, "hero");
forge::savePrefab(hero, "hero.prefab.json");
forge::Prefab loadedPrefab;
forge::loadPrefab(loadedPrefab, "hero.prefab.json");
forge::Entity e = forge::instantiate(scene, loadedPrefab, {100, 200}, &engine.assets());
```

- Format: `{"format":"forge.scene","version":1,"entities":[...]}`. A prefab is
  the same entity object without the wrapper, so it is also hand-editable.
- Serialized components: `Transform`, `Tag`, `Sprite`, `Velocity`, `Collider`,
  `ScriptRef`. `Script` closures are **not** serialized — re-attach them after
  `loadScene` by matching `ScriptRef::name`.
- Sprite textures are stored as `Texture2D::path()` and resolved through the
  `AssetManager` (its search paths apply). With no `AssetManager` the sprite
  keeps its rect but gets a null texture.
- `instantiate` overrides the stored Transform position; `entityToJson`/
  `entityFromJson` are the primitives if you want entity data embedded in your
  own save format.
- Loading is strict: a wrong `format`, a missing file or malformed JSON returns
  false and leaves the target scene untouched (no exceptions anywhere).
