#pragma once
// Physics: AABB/circle bodies, spatial-hash broadphase, impulse-free arcade
// solver. Deterministic, allocation-free step (broadphase storage is reused
// across substeps).
//
// Arcade-grade, NOT a full rigid-body engine: unit masses, no rotation/torque,
// a single relaxation pass per substep (deep stacks may jitter), discrete
// collision (clamped dt + speed-derived substeps resist tunneling).
// Tuned for platformers / top-down 2D.
//
// Feature set:
// - box and circle shapes (`Body::shape`)
// - collision layers/masks (`Body::layer`, `Body::mask`)
// - sensors with enter/exit events (`onTriggerEnter`, `onTriggerExit`)
// - one-way platforms (`Body::oneWay`)
// - moving-platform carry (riders inherit the support's delta)
// - distance and spring joints (`addJoint`)
// - continuous-ish collision via speed-derived substeps
// - `debugDraw()` for visualizing everything above

#include "forge/math/math.hpp"
#include <vector>
#include <functional>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace forge {

class SpriteBatch;
class Font;

enum class BodyType { Static, Kinematic, Dynamic };
enum class ShapeType { Box, Circle };

struct Body {
    uint32_t id = 0;
    BodyType type = BodyType::Dynamic;
    ShapeType shape = ShapeType::Box;
    Vec2 pos{0, 0};        // center
    Vec2 prevPos{0, 0};    // center at the start of the last substep
    Vec2 vel{0, 0};
    Vec2 size{32, 32};     // full extents for Box (aabb() is centered on pos)
    float radius = 16.0f;  // Circle only
    float gravityScale = 1.0f;
    float friction = 0.0f; // ground friction applied by World when onGround
    float restitution = 0.0f;
    bool onGround = false;
    bool sensor = false;   // reports overlap, no resolve
    bool oneWay = false;   // solid only when entered from the top
    // Collision filtering: a pair collides when (a.layer & b.mask) and
    // (b.layer & a.mask) are both non-zero. Defaults: everything collides.
    uint32_t layer = 0xFFFFFFFFu;
    uint32_t mask = 0xFFFFFFFFu;
    void* userData = nullptr;

    Rect aabb() const { return Rect::fromCenter(pos, size); }
    // Tight bounds for either shape (circle uses radius).
    Rect bounds() const {
        if (shape == ShapeType::Circle)
            return Rect::fromCenter(pos, {radius * 2.0f, radius * 2.0f});
        return aabb();
    }
};

struct Contact {
    uint32_t a = 0, b = 0;
    Vec2 normal{0, 0}; // from b to a (the direction a is pushed out)
    float penetration = 0;
    Vec2 point{0, 0};  // approximate contact point (mid-overlap)
    bool sensor = false;
};

using ContactCallback = std::function<void(const Contact&)>;
using TriggerCallback = std::function<void(const Contact&)>;

enum class JointType { Distance, Spring };

struct Joint {
    uint32_t id = 0;
    uint32_t a = 0, b = 0;
    JointType type = JointType::Distance;
    float restLength = 0.0f; // 0 = use the current distance at add time
    float stiffness = 1.0f;  // 0..1 position correction factor
    float damping = 0.0f;    // 0..1 velocity damping along the axis
    bool active = true;
};

struct PhysicsDebugOptions {
    bool bodies = true;
    bool sensors = true;
    bool joints = false;
    bool contacts = false;
    bool velocities = false;
    bool labels = false;
    float thickness = 1.0f;
    const Font* font = nullptr; // required for labels; null = no text
    Color textColor = Color::white();
};

class PhysicsWorld {
public:
    PhysicsWorld() = default;

    void setGravity(Vec2 g) { gravity_ = g; }
    Vec2 gravity() const { return gravity_; }
    void setCellSize(float s) { // ignore <= 0; stale grid is rebuilt lazily
        if (s > 0) { cellSize_ = s; gridDirty_ = true; }
    }
    // Enables speed-derived substeps (default on). Caps at 16.
    void setContinuous(bool on) { continuous_ = on; }
    bool continuous() const { return continuous_; }

    uint32_t addBody(const Body& b);
    void removeBody(uint32_t id);
    void clear();
    // NOTE: returned pointers alias internal storage; any addBody/removeBody
    // may reallocate and invalidate them. Re-fetch after structural changes,
    // or copy what you need.
    Body* get(uint32_t id);
    const Body* get(uint32_t id) const;
    size_t bodyCount() const { return bodies_.size(); }
    // Iteration in storage order (stable within a step).
    void forEachBody(const std::function<void(const Body&)>& fn) const;

    void onContact(ContactCallback cb) { contactCb_ = std::move(cb); }
    // Fired once when a sensor overlap starts / ends (after step()).
    void onTriggerEnter(TriggerCallback cb) { triggerEnterCb_ = std::move(cb); }
    void onTriggerExit(TriggerCallback cb) { triggerExitCb_ = std::move(cb); }

    // Fixed-step friendly. Substeps internally for stability.
    void step(float dt);

    // Queries. queryRect reuses the broadphase grid, rebuilt lazily after a
    // step, a structural change, or a non-const get() (a write opportunity);
    // no full linear scan. The const overload does not invalidate.
    std::vector<uint32_t> queryRect(Rect r) const;
    // Nearest non-sensor entry surface along the segment. Bodies that contain
    // the ray origin are ignored (they would otherwise report a hit at t = 0).
    bool raycast(Vec2 from, Vec2 to, uint32_t* hitId = nullptr, Vec2* hitPos = nullptr) const;

    // ---- joints ----
    uint32_t addJoint(const Joint& j);
    void removeJoint(uint32_t id);
    size_t jointCount() const { return joints_.size(); }

    // ---- debug ----
    // Draws wireframes in world space. Call between batch.begin()/end().
    void debugDraw(SpriteBatch& batch, const PhysicsDebugOptions& opts = {}) const;

private:
    void broadphase();        // fills pairs_ from grid_ (reused; no steady-state allocation)
    void rebuildGrid() const; // fills grid_ for current AABBs, clears gridDirty_
    static bool collide(const Body& a, const Body& b, Contact& out);
    static int64_t cellKey(int x, int y);
    static bool filterAllows(const Body& a, const Body& b) {
        return (a.layer & b.mask) != 0 && (b.layer & a.mask) != 0;
    }
    void solveJoints();
    void emitTriggerEvents();

    Vec2 gravity_{0, -980.0f};
    float cellSize_ = 64.0f;
    bool continuous_ = true;
    std::vector<Body> bodies_;
    std::vector<Joint> joints_;
    std::unordered_map<uint32_t, size_t> byId_; // body id -> index in bodies_
    uint32_t nextId_ = 1;
    uint32_t nextJointId_ = 1;
    ContactCallback contactCb_;
    TriggerCallback triggerEnterCb_, triggerExitCb_;
    // Broadphase scratch, kept across steps so steady-state step() does not
    // allocate. Bucket vectors are cleared (capacity retained) each step.
    // Mutable so const queries (queryRect) can lazily rebuild the grid.
    mutable std::unordered_map<int64_t, std::vector<int>> grid_;
    mutable bool gridDirty_ = true;
    std::vector<std::pair<int, int>> pairs_;
    mutable std::vector<int> queryScratch_; // deduped candidates for queryRect
    // Sensor pairs seen in the previous step, keyed by unordered body-id pair.
    std::unordered_set<uint64_t> activeTriggers_;
    std::unordered_set<uint64_t> nextTriggers_;
    std::vector<Contact> stepContacts_; // for debug draw
    // Per-substep support tracking for moving-platform carry.
    std::unordered_map<uint32_t, uint32_t> support_; // rider id -> support id
};

} // namespace forge
