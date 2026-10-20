#include "forge/physics/physics.hpp"
#include "forge/graphics/sprite_batch.hpp"
#include "forge/graphics/font.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace forge {

namespace {
uint64_t pairKey(uint32_t a, uint32_t b) {
    if (a > b) std::swap(a, b);
    return ((uint64_t)a << 32) | b;
}
} // namespace

uint32_t PhysicsWorld::addBody(const Body& b) {
    Body nb = b;
    nb.id = nextId_++;
    nb.onGround = false;
    nb.prevPos = nb.pos;
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
    // Joints referencing the removed body die with it.
    joints_.erase(std::remove_if(joints_.begin(), joints_.end(),
                                 [id](const Joint& j) { return j.a == id || j.b == id; }),
                  joints_.end());
    support_.erase(id);
    for (auto jt = support_.begin(); jt != support_.end();)
        jt = (jt->second == id) ? support_.erase(jt) : std::next(jt);
    gridDirty_ = true;
}

void PhysicsWorld::clear() {
    bodies_.clear();
    joints_.clear();
    byId_.clear();
    support_.clear();
    activeTriggers_.clear();
    nextTriggers_.clear();
    stepContacts_.clear();
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

void PhysicsWorld::forEachBody(const std::function<void(const Body&)>& fn) const {
    for (const auto& b : bodies_) fn(b);
}

int64_t PhysicsWorld::cellKey(int x, int y) {
    return ((int64_t)(uint32_t)x << 32) | (uint32_t)y;
}

// Rebuilds grid_ for the current body AABBs. Bucket vectors keep their
// capacity, so after warm-up this does not allocate.
void PhysicsWorld::rebuildGrid() const {
    for (auto& kv : grid_) kv.second.clear();
    for (size_t i = 0; i < bodies_.size(); ++i) {
        Rect r = bodies_[i].bounds();
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
    Rect ra = a.bounds(), rb = b.bounds();
    if (!ra.overlaps(rb)) return false;

    out.a = a.id; out.b = b.id;
    out.sensor = a.sensor || b.sensor;

    if (a.shape == ShapeType::Circle && b.shape == ShapeType::Circle) {
        Vec2 d = a.pos - b.pos;
        float dist = d.length();
        float r = a.radius + b.radius;
        if (dist >= r) return false;
        out.penetration = r - dist;
        out.normal = dist > EPSILON ? d / dist : Vec2{0, 1};
        out.point = b.pos + out.normal * b.radius;
        return true;
    }
    if (a.shape == ShapeType::Circle || b.shape == ShapeType::Circle) {
        // Circle vs box: resolve on the circle and flip for the box side.
        const Body& circle = (a.shape == ShapeType::Circle) ? a : b;
        const Body& box = (a.shape == ShapeType::Circle) ? b : a;
        Vec2 push = resolveCircleRect(Circle{circle.pos, circle.radius}, box.aabb());
        if (push.lengthSq() <= EPSILON) return false;
        Vec2 n = push.normalized(); // direction to push the circle
        out.penetration = push.length();
        out.normal = (a.shape == ShapeType::Circle) ? n : -n;
        out.point = circle.pos - n * (circle.radius - out.penetration * 0.5f);
        return true;
    }

    // Box vs box (AABB MTV).
    float dx1 = (rb.x + rb.w) - ra.x;  // push a -x
    float dx2 = ra.x + ra.w - rb.x;
    float dy1 = (rb.y + rb.h) - ra.y;
    float dy2 = ra.y + ra.h - rb.y;
    float mx = std::min(dx1, dx2), my = std::min(dy1, dy2);
    if (mx < my) {
        out.penetration = mx;
        out.normal = (dx1 < dx2) ? Vec2{1, 0} : Vec2{-1, 0};
    } else {
        out.penetration = my;
        out.normal = (dy1 < dy2) ? Vec2{0, 1} : Vec2{0, -1};
    }
    out.point = {std::max(ra.x, rb.x) + std::min(ra.x + ra.w, rb.x + rb.w) * 0.0f,
                 std::max(ra.y, rb.y)};
    // Midpoint of the overlap rect is a better visual contact point.
    float ox0 = std::max(ra.x, rb.x), ox1 = std::min(ra.x + ra.w, rb.x + rb.w);
    float oy0 = std::max(ra.y, rb.y), oy1 = std::min(ra.y + ra.h, rb.y + rb.h);
    out.point = {(ox0 + ox1) * 0.5f, (oy0 + oy1) * 0.5f};
    return true;
}

void PhysicsWorld::step(float dt) {
    if (dt <= 0) return;
    if (dt > 0.1f) dt = 0.1f; // clamp hitches: no tunneling past this budget
    // Scale substeps so no body moves more than ~1/120s worth per substep.
    int sub = (int)std::ceil(dt / (1.0f / 120.0f));
    if (continuous_) {
        // CCD-ish: also account for the fastest body crossing the smallest
        // extent in a single substep. Capped at 16 for cost.
        float maxSpeed = 0, minExtent = 1e9f;
        for (auto& b : bodies_) {
            if (b.type != BodyType::Dynamic) continue;
            maxSpeed = std::max(maxSpeed, b.vel.length());
            float ext = b.shape == ShapeType::Circle ? b.radius * 2.0f
                                                     : std::min(b.size.x, b.size.y);
            minExtent = std::min(minExtent, std::max(ext, 1.0f));
        }
        if (maxSpeed > 0 && minExtent < 1e8f) {
            int need = (int)std::ceil(maxSpeed * dt / (minExtent * 0.5f));
            sub = std::max(sub, need);
        }
    }
    if (sub < 1) sub = 1;
    if (sub > 16) sub = 16;
    float h = dt / sub;

    stepContacts_.clear();
    nextTriggers_.clear();

    for (int s = 0; s < sub; ++s) {
        // integrate
        for (auto& b : bodies_) {
            b.prevPos = b.pos;
            b.onGround = false;
            if (b.type == BodyType::Static) continue;
            if (b.type == BodyType::Dynamic) {
                b.vel += gravity_ * b.gravityScale * h;
            }
            b.pos += b.vel * h;
        }

        // Moving-platform carry: riders inherit the support's movement before
        // resolution so they do not sink into it or get left behind.
        for (auto& [riderId, supportId] : support_) {
            Body* rider = get(riderId);
            const Body* sup = get(supportId);
            if (!rider || !sup) continue;
            if (rider->type != BodyType::Dynamic) continue;
            Vec2 delta = sup->pos - sup->prevPos;
            if (delta.lengthSq() > 0) rider->pos += delta;
        }

        // collide
        broadphase();
        for (auto [i, j] : pairs_) {
            Body& a = bodies_[(size_t)i];
            Body& b = bodies_[(size_t)j];
            if (!filterAllows(a, b)) continue;
            bool aStat = a.type == BodyType::Static;
            bool bStat = b.type == BodyType::Static;
            // Static-static pairs are at rest; still report sensors, else skip.
            if (aStat && bStat && !a.sensor && !b.sensor) continue;
            Contact c;
            if (!collide(a, b, c)) continue;

            // One-way: solid only when the dynamic body comes from above.
            if (a.oneWay || b.oneWay) {
                bool blocked;
                if (a.oneWay && b.oneWay) {
                    blocked = true; // two one-way bodies: treat as solid
                } else if (a.oneWay) {
                    // b is the mover, approaching a from above (a pushes b up).
                    blocked = c.normal.y < 0.0f &&
                              b.prevPos.y >= a.pos.y + a.size.y * 0.5f - 1.0f;
                } else {
                    // a is the mover, approaching b from above.
                    blocked = c.normal.y > 0.0f &&
                              a.prevPos.y >= b.pos.y + b.size.y * 0.5f - 1.0f;
                }
                if (!blocked) continue;
            }

            if (c.sensor) {
                nextTriggers_.insert(pairKey(a.id, b.id));
            }
            stepContacts_.push_back(c);
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
            // normal points from b to a: a is pushed along it, b against it
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
            // Record who supports whom for the next substep's carry pass.
            if (n.y > 0.7f && aDyn && (bStat || bKin)) support_[a.id] = b.id;
            else if (n.y < -0.7f && bDyn && (aStat || aKin)) support_[b.id] = a.id;
        }

        solveJoints();
    }

    // Drop stale support entries (support moved away / body no longer above).
    for (auto it = support_.begin(); it != support_.end();) {
        const Body* rider = get(it->first);
        const Body* sup = get(it->second);
        bool keep = rider && sup && rider->onGround;
        it = keep ? std::next(it) : support_.erase(it);
    }

    // Contacts moved bodies after the last broadphase: the cached query grid
    // no longer matches the final positions, so mark it stale.
    gridDirty_ = true;

    emitTriggerEvents();
}

void PhysicsWorld::solveJoints() {
    for (auto& j : joints_) {
        if (!j.active) continue;
        Body* a = get(j.a);
        Body* b = get(j.b);
        if (!a || !b) continue;
        Vec2 d = b->pos - a->pos;
        float dist = d.length();
        if (dist < EPSILON) continue;
        Vec2 axis = d / dist;
        float rest = j.restLength > 0 ? j.restLength : dist;
        float err = dist - rest;
        // Unit masses for dynamic bodies; static/kinematic are immovable.
        bool aMov = a->type == BodyType::Dynamic;
        bool bMov = b->type == BodyType::Dynamic;
        float invA = aMov ? 1.0f : 0.0f;
        float invB = bMov ? 1.0f : 0.0f;
        float sum = invA + invB;
        if (sum <= 0) continue;
        float k = clamp01(j.stiffness);
        Vec2 corr = axis * (err * k / sum);
        a->pos += corr * invA;
        b->pos -= corr * invB;
        // Damping: remove relative velocity along the axis.
        float rel = (b->vel - a->vel).dot(axis);
        float damp = clamp01(j.damping);
        if (damp > 0) {
            float jimp = rel * damp / sum;
            a->vel += axis * (jimp * invA);
            b->vel -= axis * (jimp * invB);
        }
    }
}

void PhysicsWorld::emitTriggerEvents() {
    // Enter: in next but not in active. Exit: in active but not in next.
    for (uint64_t key : nextTriggers_) {
        if (activeTriggers_.count(key)) continue;
        if (!triggerEnterCb_) continue;
        Contact c;
        c.a = (uint32_t)(key >> 32);
        c.b = (uint32_t)(key & 0xFFFFFFFFu);
        c.sensor = true;
        const Body* a = get(c.a);
        const Body* b = get(c.b);
        if (a && b) {
            c.point = (a->pos + b->pos) * 0.5f;
            c.normal = (a->pos - b->pos).normalized();
        }
        triggerEnterCb_(c);
    }
    for (uint64_t key : activeTriggers_) {
        if (nextTriggers_.count(key)) continue;
        if (!triggerExitCb_) continue;
        Contact c;
        c.a = (uint32_t)(key >> 32);
        c.b = (uint32_t)(key & 0xFFFFFFFFu);
        c.sensor = true;
        triggerExitCb_(c);
    }
    activeTriggers_.swap(nextTriggers_);
    nextTriggers_.clear();
}

uint32_t PhysicsWorld::addJoint(const Joint& j) {
    Joint nj = j;
    nj.id = nextJointId_++;
    if (nj.restLength <= 0) {
        const Body* a = get(j.a);
        const Body* b = get(j.b);
        if (a && b) nj.restLength = (b->pos - a->pos).length();
    }
    joints_.push_back(nj);
    return nj.id;
}

void PhysicsWorld::removeJoint(uint32_t id) {
    joints_.erase(std::remove_if(joints_.begin(), joints_.end(),
                                 [id](const Joint& j) { return j.id == id; }),
                  joints_.end());
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
        if (bodies_[(size_t)i].bounds().overlaps(r)) out.push_back(bodies_[(size_t)i].id);
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
        Rect r = b.bounds();
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

void PhysicsWorld::debugDraw(SpriteBatch& batch, const PhysicsDebugOptions& opts) const {
    Color colStatic = Color::fromHex(0x6E7B8BFF);
    Color colDynamic = Color::fromHex(0x4FC3F7FF);
    Color colKinematic = Color::fromHex(0xFFB74DFF);
    Color colSensor = Color::fromHex(0xBA68C8FF);
    Color colJoint = Color::fromHex(0xFFF176FF);

    if (opts.bodies) {
        for (auto& b : bodies_) {
            Color c = b.sensor ? colSensor
                      : (b.type == BodyType::Static ? colStatic
                         : (b.type == BodyType::Kinematic ? colKinematic : colDynamic));
            if (b.sensor) c.a = 0.7f;
            if (b.shape == ShapeType::Circle) {
                batch.drawCircleOutline(b.pos, b.radius, c, opts.thickness, 24);
            } else {
                batch.drawRectOutline(b.aabb(), c, opts.thickness);
            }
            if (opts.velocities && b.vel.lengthSq() > 1.0f) {
                batch.drawLine(b.pos, b.pos + b.vel * 0.1f, c, opts.thickness);
            }
            if (opts.labels && opts.font) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%u%s", b.id, b.onGround ? " g" : "");
                opts.font->drawCentered(batch, buf, {b.pos.x, b.pos.y + b.size.y * 0.5f + 8.0f},
                                        0.5f, opts.textColor);
            }
        }
    }
    if (opts.joints) {
        for (auto& j : joints_) {
            const Body* a = get(j.a);
            const Body* b = get(j.b);
            if (!a || !b) continue;
            batch.drawLine(a->pos, b->pos, colJoint, opts.thickness);
        }
    }
    if (opts.contacts) {
        for (auto& c : stepContacts_) {
            batch.drawCircle(c.point, 2.0f, Color::yellow());
            batch.drawLine(c.point, c.point + c.normal * 10.0f, Color::yellow(), 1.0f);
        }
    }
}

} // namespace forge
