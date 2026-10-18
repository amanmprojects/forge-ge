#include "forge/physics/physics.hpp"
#include <unordered_map>
#include <algorithm>
#include <cmath>

namespace forge {

uint32_t PhysicsWorld::addBody(const Body& b) {
    Body nb = b;
    nb.id = nextId_++;
    nb.onGround = false;
    bodies_.push_back(nb);
    byId_[nb.id] = bodies_.size() - 1;
    gridDirty_ = true;
    return nb.id;
}

void PhysicsWorld::removeBody(uint32_t id) {
    auto it = byId_.find(id);
    if (it == byId_.end()) return;
    size_t idx = it->second;
    Body moved = bodies_.back();      // swap-erase: O(1), order-stable enough
    bodies_[idx] = moved;
    bodies_.pop_back();
    byId_.erase(it);
    if (idx < bodies_.size()) byId_[moved.id] = idx;
    gridDirty_ = true;
}

void PhysicsWorld::clear() {
    bodies_.clear();
    byId_.clear();
    gridDirty_ = true;
}

Body* PhysicsWorld::get(uint32_t id) {
    // A non-const handle is a write opportunity (pos/size/...): treat the
    // cached query grid as stale so the next queryRect() reflects the edit.
    gridDirty_ = true;
    auto it = byId_.find(id);
    return it == byId_.end() ? nullptr : &bodies_[it->second];
}
const Body* PhysicsWorld::get(uint32_t id) const {
    auto it = byId_.find(id);
    return it == byId_.end() ? nullptr : &bodies_[it->second];
}

int64_t PhysicsWorld::cellKey(int x, int y) {
    return ((int64_t)(uint32_t)x << 32) | (uint32_t)y;
}

// Rebuilds grid_ for the current body AABBs. Bucket vectors keep their
// capacity, so after warm-up this does not allocate.
void PhysicsWorld::rebuildGrid() const {
    for (auto& kv : grid_) kv.second.clear();
    for (size_t i = 0; i < bodies_.size(); ++i) {
        Rect r = bodies_[i].aabb();
        int x0 = (int)std::floor(r.x / cellSize_), x1 = (int)std::floor((r.x + r.w) / cellSize_);
        int y0 = (int)std::floor(r.y / cellSize_), y1 = (int)std::floor((r.y + r.h) / cellSize_);
        for (int cy = y0; cy <= y1; ++cy)
            for (int cx = x0; cx <= x1; ++cx)
                grid_[cellKey(cx, cy)].push_back((int)i);
    }
    gridDirty_ = false;
}

// Fills the reusable pairs_ scratch from grid_. No steady-state allocation.
void PhysicsWorld::broadphase() {
    pairs_.clear();
    rebuildGrid();
    for (auto& kv : grid_) {
        auto& cell = kv.second;
        for (size_t a = 0; a < cell.size(); ++a)
            for (size_t b = a + 1; b < cell.size(); ++b) {
                int i = cell[a], j = cell[b];
                if (i > j) std::swap(i, j);
                pairs_.emplace_back(i, j);
            }
    }
    // A pair straddling several cells is emitted once per shared cell.
    std::sort(pairs_.begin(), pairs_.end());
    pairs_.erase(std::unique(pairs_.begin(), pairs_.end()), pairs_.end());
}

bool PhysicsWorld::collide(const Body& a, const Body& b, Contact& out) {
    Rect ra = a.aabb(), rb = b.aabb();
    if (!ra.overlaps(rb)) return false;
    float dx1 = (rb.x + rb.w) - ra.x;  // push a -x? compute MTV for a
    float dx2 = ra.x + ra.w - rb.x;
    float dy1 = (rb.y + rb.h) - ra.y;
    float dy2 = ra.y + ra.h - rb.y;
    float mx = std::min(dx1, dx2), my = std::min(dy1, dy2);
    out.a = a.id; out.b = b.id;
    if (mx < my) {
        out.penetration = mx;
        out.normal = (dx1 < dx2) ? Vec2{1, 0} : Vec2{-1, 0};
    } else {
        out.penetration = my;
        out.normal = (dy1 < dy2) ? Vec2{0, 1} : Vec2{0, -1};
    }
    return true;
}

void PhysicsWorld::step(float dt) {
    if (dt <= 0) return;
    if (dt > 0.1f) dt = 0.1f; // clamp hitches: no tunneling past this budget
    // Scale substeps so no body moves more than ~1/120s worth per substep.
    int sub = (int)std::ceil(dt / (1.0f / 120.0f));
    if (sub < 1) sub = 1;
    if (sub > 8) sub = 8;
    float h = dt / sub;

    for (int s = 0; s < sub; ++s) {
        // integrate
        for (auto& b : bodies_) {
            b.onGround = false;
            if (b.type == BodyType::Static) continue;
            if (b.type == BodyType::Dynamic) {
                b.vel += gravity_ * b.gravityScale * h;
            }
            b.pos += b.vel * h;
        }

        // collide
        broadphase();
        for (auto [i, j] : pairs_) {
            Body& a = bodies_[(size_t)i];
            Body& b = bodies_[(size_t)j];
            bool aStat = a.type == BodyType::Static;
            bool bStat = b.type == BodyType::Static;
            // Static-static pairs are at rest; still report sensors, else skip.
            if (aStat && bStat && !a.sensor && !b.sensor) continue;
            Contact c;
            if (!collide(a, b, c)) continue;
            if (contactCb_) contactCb_(c); // fires for sensors too (incl. static)
            if (a.sensor || b.sensor) continue;

            // Inverse masses. Static = infinite. Kinematic = infinite vs
            // dynamic (pushes them), finite vs static/kinematic (gets pushed
            // out instead of tunneling through).
            bool aDyn = a.type == BodyType::Dynamic;
            bool bDyn = b.type == BodyType::Dynamic;
            bool aKin = a.type == BodyType::Kinematic;
            bool bKin = b.type == BodyType::Kinematic;
            float invA = aDyn ? 1.0f : (aKin && !bDyn ? 1.0f : 0.0f);
            float invB = bDyn ? 1.0f : (bKin && !aDyn ? 1.0f : 0.0f);
            float sum = invA + invB;
            if (sum <= 0) continue;
            Vec2 corr = c.normal * (c.penetration / sum);
            // normal points from b-side... define: collide() normal moves a out of b
            a.pos += corr * invA;
            b.pos -= corr * invB;

            // kill inbound velocity + restitution
            float rest = std::max(a.restitution, b.restitution);
            Vec2 n = c.normal;
            float va = a.vel.dot(n), vb = b.vel.dot(n);
            float rel = va - vb;
            if (rel < 0) {
                float jimp = -(1.0f + rest) * rel / sum;
                a.vel += n * (jimp * invA);
                b.vel -= n * (jimp * invB);
            }
            // ground flags + friction (friction scaled by substep so the
            // result is frame-rate independent: f*h*60 ~= f at 60 Hz)
            if (n.y > 0.7f && aDyn) {
                a.onGround = true;
                a.vel.x *= (1.0f - clamp01(a.friction * h * 60.0f));
            }
            if (n.y < -0.7f && bDyn) {
                b.onGround = true;
                b.vel.x *= (1.0f - clamp01(b.friction * h * 60.0f));
            }
        }
    }
    // Contacts moved bodies after the last broadphase: the cached query grid
    // no longer matches the final positions, so mark it stale.
    gridDirty_ = true;
}

std::vector<uint32_t> PhysicsWorld::queryRect(Rect r) const {
    if (gridDirty_) rebuildGrid();
    // Candidate indices from overlapped cells (dedup: a body spans several
    // cells, and AABB overlap needs exact testing anyway).
    queryScratch_.clear();
    int x0 = (int)std::floor(r.x / cellSize_), x1 = (int)std::floor((r.x + r.w) / cellSize_);
    int y0 = (int)std::floor(r.y / cellSize_), y1 = (int)std::floor((r.y + r.h) / cellSize_);
    for (int cy = y0; cy <= y1; ++cy)
        for (int cx = x0; cx <= x1; ++cx) {
            auto it = grid_.find(cellKey(cx, cy));
            if (it == grid_.end()) continue;
            for (int i : it->second) queryScratch_.push_back(i);
        }
    std::sort(queryScratch_.begin(), queryScratch_.end());
    queryScratch_.erase(std::unique(queryScratch_.begin(), queryScratch_.end()),
                        queryScratch_.end());
    std::vector<uint32_t> out;
    out.reserve(queryScratch_.size());
    for (int i : queryScratch_)
        if (bodies_[(size_t)i].aabb().overlaps(r)) out.push_back(bodies_[(size_t)i].id);
    return out;
}

bool PhysicsWorld::raycast(Vec2 from, Vec2 to, uint32_t* hitId, Vec2* hitPos) const {
    // slab test per body, nearest entry t wins. A ray whose ORIGIN is inside
    // a body skips that body entirely (tmin == 0 would make every enclosing
    // box a hit at distance 0 and hide the surface further along the ray).
    Vec2 d = to - from;
    float best = 1.0f + EPSILON;
    const Body* bestB = nullptr;
    for (auto& b : bodies_) {
        if (b.sensor) continue;
        Rect r = b.aabb();
        float tmin = 0.0f, tmax = 1.0f;
        bool ok = true;
        // X slab
        if (std::fabs(d.x) < EPSILON) {
            if (from.x < r.x || from.x > r.x + r.w) ok = false;
        } else {
            float t1 = (r.x - from.x) / d.x, t2 = (r.x + r.w - from.x) / d.x;
            if (t1 > t2) std::swap(t1, t2);
            tmin = std::max(tmin, t1); tmax = std::min(tmax, t2);
        }
        // Y slab
        if (ok) {
            if (std::fabs(d.y) < EPSILON) {
                if (from.y < r.y || from.y > r.y + r.h) ok = false;
            } else {
                float t1 = (r.y - from.y) / d.y, t2 = (r.y + r.h - from.y) / d.y;
                if (t1 > t2) std::swap(t1, t2);
                tmin = std::max(tmin, t1); tmax = std::min(tmax, t2);
            }
        }
        if (ok && tmin <= tmax && tmin > 0.0f && tmin < best) {
            best = tmin;
            bestB = &b;
        }
    }
    if (bestB) {
        if (hitId) *hitId = bestB->id;
        if (hitPos) *hitPos = from + d * best;
        return true;
    }
    return false;
}

} // namespace forge
