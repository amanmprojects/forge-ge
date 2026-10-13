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
    return nb.id;
}

void PhysicsWorld::removeBody(uint32_t id) {
    bodies_.erase(std::remove_if(bodies_.begin(), bodies_.end(),
        [id](const Body& b) { return b.id == id; }), bodies_.end());
}

void PhysicsWorld::clear() { bodies_.clear(); }

Body* PhysicsWorld::get(uint32_t id) {
    for (auto& b : bodies_) if (b.id == id) return &b;
    return nullptr;
}
const Body* PhysicsWorld::get(uint32_t id) const {
    for (auto& b : bodies_) if (b.id == id) return &b;
    return nullptr;
}

static int64_t cellKey(int x, int y) {
    return ((int64_t)(uint32_t)x << 32) | (uint32_t)y;
}

void PhysicsWorld::broadphase(std::vector<std::pair<int,int>>& pairs) const {
    pairs.clear();
    std::unordered_map<int64_t, std::vector<int>> grid;
    grid.reserve(bodies_.size() * 2);

    for (size_t i = 0; i < bodies_.size(); ++i) {
        Rect r = bodies_[i].aabb();
        int x0 = (int)std::floor(r.x / cellSize_), x1 = (int)std::floor((r.x + r.w) / cellSize_);
        int y0 = (int)std::floor(r.y / cellSize_), y1 = (int)std::floor((r.y + r.h) / cellSize_);
        for (int cy = y0; cy <= y1; ++cy)
            for (int cx = x0; cx <= x1; ++cx)
                grid[cellKey(cx, cy)].push_back((int)i);
    }
    for (auto& kv : grid) {
        auto& cell = kv.second;
        for (size_t a = 0; a < cell.size(); ++a)
            for (size_t b = a + 1; b < cell.size(); ++b) {
                int i = cell[a], j = cell[b];
                if (i > j) std::swap(i, j);
                pairs.emplace_back(i, j);
            }
    }
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
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
    // substep for stability at low fps
    int sub = dt > 1.0f / 90.0f ? 2 : 1;
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
        std::vector<std::pair<int,int>> pairs;
        broadphase(pairs);
        for (auto [i, j] : pairs) {
            Body& a = bodies_[(size_t)i];
            Body& b = bodies_[(size_t)j];
            if (a.type == BodyType::Static && b.type == BodyType::Static) continue;
            Contact c;
            if (!collide(a, b, c)) continue;
            if (contactCb_) contactCb_(c);

            bool aDyn = a.type == BodyType::Dynamic;
            bool bDyn = b.type == BodyType::Dynamic;
            bool aKin = a.type == BodyType::Kinematic;
            bool bKin = b.type == BodyType::Kinematic;
            if (a.sensor || b.sensor) continue;
            if (!aDyn && !bDyn && !aKin && !bKin) continue;

            // positional correction, weighted by inverse mass (static = infinite mass)
            float invA = aDyn ? 1.0f : 0.0f;
            float invB = bDyn ? 1.0f : 0.0f;
            // kinematics push dynamics fully
            if (aKin && bDyn) { invA = 0; invB = 1; }
            if (bKin && aDyn) { invA = 1; invB = 0; }
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
            // ground flags + friction
            if (n.y > 0.7f && aDyn) { a.onGround = true; a.vel.x *= (1.0f - clamp01(a.friction)); }
            if (n.y < -0.7f && bDyn) { b.onGround = true; b.vel.x *= (1.0f - clamp01(b.friction)); }
        }
    }
}

std::vector<uint32_t> PhysicsWorld::queryRect(Rect r) const {
    std::vector<uint32_t> out;
    for (auto& b : bodies_) {
        if (b.aabb().overlaps(r)) out.push_back(b.id);
    }
    return out;
}

bool PhysicsWorld::raycast(Vec2 from, Vec2 to, uint32_t* hitId, Vec2* hitPos) const {
    // slab test per body, nearest t wins
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
        if (ok && tmin <= tmax && tmin < best) {
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
