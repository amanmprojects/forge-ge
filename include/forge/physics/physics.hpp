#pragma once
// Physics: AABB bodies, spatial-hash broadphase, impulse-free arcade solver.
// Deterministic, allocation-free step (broadphase storage is reused across
// substeps). Arcade-grade, NOT a full rigid-body engine: unit masses, no
// rotation/torque, single relaxation pass per substep (deep stacks may
// jitter), discrete collision (clamped dt + substeps resist tunneling but
// very fast small bodies can still pass).
// Tuned for platformers / top-down 2D.

#include "forge/math/math.hpp"
#include <vector>
#include <functional>
#include <cstdint>
#include <unordered_map>

namespace forge {

enum class BodyType { Static, Kinematic, Dynamic };

struct Body {
    uint32_t id = 0;
    BodyType type = BodyType::Dynamic;
    Vec2 pos{0, 0};        // center
    Vec2 vel{0, 0};
    Vec2 size{32, 32};     // full extents (aabb() is centered on pos)
    float gravityScale = 1.0f;
    float friction = 0.0f; // ground friction applied by World when onGround
    float restitution = 0.0f;
    bool onGround = false;
    bool sensor = false;   // reports overlap, no resolve
    void* userData = nullptr;

    Rect aabb() const { return Rect::fromCenter(pos, size); }
};

struct Contact {
    uint32_t a = 0, b = 0;
    Vec2 normal{0, 0}; // from a to b resolution direction for a
    float penetration = 0;
};

using ContactCallback = std::function<void(const Contact&)>;

class PhysicsWorld {
public:
    PhysicsWorld() = default;

    void setGravity(Vec2 g) { gravity_ = g; }
    Vec2 gravity() const { return gravity_; }
    void setCellSize(float s) { // ignore <= 0; stale grid is rebuilt lazily
        if (s > 0) { cellSize_ = s; gridDirty_ = true; }
    }

    uint32_t addBody(const Body& b);
    void removeBody(uint32_t id);
    void clear();
    // NOTE: returned pointers alias internal storage; any addBody/removeBody
    // may reallocate and invalidate them. Re-fetch after structural changes,
    // or copy what you need.
    Body* get(uint32_t id);
    const Body* get(uint32_t id) const;
    size_t bodyCount() const { return bodies_.size(); }

    void onContact(ContactCallback cb) { contactCb_ = std::move(cb); }

    // Fixed-step friendly. Substeps internally for stability.
    void step(float dt);

    // Queries. queryRect reuses the broadphase grid, rebuilt lazily after a
    // step, a structural change, or a non-const get() (a write opportunity);
    // no full linear scan. The const overload does not invalidate.
    std::vector<uint32_t> queryRect(Rect r) const;
    // Nearest non-sensor entry surface along the segment. Bodies that contain
    // the ray origin are ignored (they would otherwise report a hit at t = 0).
    bool raycast(Vec2 from, Vec2 to, uint32_t* hitId = nullptr, Vec2* hitPos = nullptr) const;

private:
    void broadphase();        // fills pairs_ from grid_ (reused; no steady-state allocation)
    void rebuildGrid() const; // fills grid_ for current AABBs, clears gridDirty_
    static bool collide(const Body& a, const Body& b, Contact& out);
    static int64_t cellKey(int x, int y);

    Vec2 gravity_{0, -980.0f};
    float cellSize_ = 64.0f;
    std::vector<Body> bodies_;
    std::unordered_map<uint32_t, size_t> byId_; // body id -> index in bodies_
    uint32_t nextId_ = 1;
    ContactCallback contactCb_;
    // Broadphase scratch, kept across steps so steady-state step() does not
    // allocate. Bucket vectors are cleared (capacity retained) each step.
    // Mutable so const queries (queryRect) can lazily rebuild the grid.
    mutable std::unordered_map<int64_t, std::vector<int>> grid_;
    mutable bool gridDirty_ = true;
    std::vector<std::pair<int, int>> pairs_;
    mutable std::vector<int> queryScratch_; // deduped candidates for queryRect
};

} // namespace forge
