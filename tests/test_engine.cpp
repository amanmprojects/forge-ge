// Headless unit tests: math, ECS, physics, tilemap-data, animation/tween.
// No GL context needed. Returns nonzero on first failure.
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <vector>
#include <fstream>
#include <filesystem>
#include "forge/math/math.hpp"
#include "forge/scene/ecs.hpp"
#include "forge/scene/components.hpp"
#include "forge/scene/scene.hpp"
#include "forge/physics/physics.hpp"
#include "forge/graphics/tilemap.hpp"
#include "forge/graphics/animation.hpp"
#include "forge/graphics/camera.hpp"
#include "forge/core/input.hpp"
#include "forge/audio/audio.hpp"
#include "forge/graphics/atlas.hpp"
#include "forge/graphics/skeleton.hpp"
#include "forge/graphics/font.hpp"
#include "forge/graphics/lighting.hpp"
#include "forge/utils/json.hpp"
#include "forge/script/script.hpp"
#include "forge/scene/serialization.hpp"
#include "forge/assets/assets.hpp"

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } \
} while (0)
#define CHECK_CLOSE(a, b, eps) do { \
    if (std::fabs((a) - (b)) > (eps)) { \
        std::printf("FAIL %s:%d: %s (%f vs %f)\n", __FILE__, __LINE__, #a, (double)(a), (double)(b)); \
        ++failures; \
    } \
} while (0)

using namespace forge;

static void testMath() {
    Vec2 a{3, 4};
    CHECK_CLOSE(a.length(), 5.0f, 1e-5f);
    CHECK_CLOSE(a.normalized().length(), 1.0f, 1e-5f);
    CHECK(a.dot(Vec2(1, 0)) == 3.0f);
    CHECK(Vec2(1, 0).rotated(PI / 2).distTo(Vec2(0, 1)) < 1e-5f);

    Mat4 o = Mat4::ortho(-1, 1, -1, 1);
    Vec2 p = o.transformPoint(Vec2(0, 0));
    CHECK_CLOSE(p.x, 0.0f, 1e-5f);
    CHECK_CLOSE(p.y, 0.0f, 1e-5f);

    Rect r{0, 0, 10, 10};
    CHECK(r.contains(Vec2(5, 5)));
    CHECK(!r.contains(Vec2(11, 5)));
    CHECK(r.overlaps(Rect(5, 5, 10, 10)));
    CHECK(!r.overlaps(Rect(11, 11, 2, 2)));

    CHECK(circleVsRect(Circle{Vec2(5, 5), 2}, r));
    CHECK(!circleVsRect(Circle{Vec2(15, 15), 2}, r));

    Color c = Color::fromHex(0xFF0000FF);
    CHECK_CLOSE(c.r, 1.0f, 1e-3f);
    CHECK_CLOSE(c.g, 0.0f, 1e-3f);

    Random rng{42};
    CHECK(rng.nextFloat() >= 0 && rng.nextFloat() < 1.0f);

    CHECK_CLOSE(easing::outCubic(0.0f), 0.0f, 1e-5f);
    CHECK_CLOSE(easing::outCubic(1.0f), 1.0f, 1e-5f);
    CHECK(valueNoise2D(0.5f, 0.5f) >= 0 && valueNoise2D(0.5f, 0.5f) <= 1.0f);
    std::printf("math ok\n");
}

static void testEcs() {
    Registry reg;
    Entity e1 = reg.create();
    Entity e2 = reg.create();
    CHECK(e1 != e2);
    CHECK(e1 != kNullEntity && e2 != kNullEntity); // 0 is never issued
    CHECK(!reg.alive(kNullEntity));
    CHECK(reg.alive(e1));

    reg.emplace<Transform>(e1, Transform{{10, 20}});
    reg.emplace<Velocity>(e1);
    reg.emplace<Transform>(e2);
    CHECK(reg.has<Transform>(e1));
    CHECK(!reg.has<Velocity>(e2));
    CHECK_CLOSE(reg.get<Transform>(e1).pos.x, 10.0f, 1e-6f);

    int count = 0;
    reg.view<Transform>([&](Entity, Transform&) { ++count; });
    CHECK(count == 2);
    count = 0;
    reg.view<Transform, Velocity>([&](Entity, Transform&, Velocity&) { ++count; });
    CHECK(count == 1);

    reg.destroy(e1);
    CHECK(!reg.alive(e1));
    CHECK(!reg.has<Transform>(e1)); // components cleaned
    Entity e3 = reg.create();       // index recycled, gen bumped
    CHECK(e3 != e1);
    CHECK(reg.alive(e3));

    // Single-element pool: destroy the TAIL (sole) component holder, then
    // query it. Old removeAt corrupted sparse_ and crashed/ASan-fired here.
    {
        Registry r2;
        Entity solo = r2.create();
        r2.emplace<Velocity>(solo);
        CHECK(r2.has<Velocity>(solo));
        r2.destroy(solo);
        CHECK(!r2.alive(solo));
        CHECK(!r2.has<Velocity>(solo)); // OOB read before fix
        // recycle still works after tail remove
        Entity n1 = r2.create();
        CHECK(n1 != solo && r2.alive(n1));
        r2.emplace<Velocity>(n1, Velocity{{1, 2}});
        CHECK_CLOSE(r2.get<Velocity>(n1).v.x, 1.0f, 1e-6f);
    }
    // Tail remove from multi-element pool keeps siblings intact
    {
        Registry r3;
        Entity a = r3.create(), b = r3.create(), c = r3.create();
        r3.emplace<Transform>(a); r3.emplace<Transform>(b); r3.emplace<Transform>(c);
        r3.get<Transform>(a).pos = {1, 0};
        r3.get<Transform>(b).pos = {2, 0};
        r3.destroy(c); // tail of the Transform pool
        CHECK(!r3.has<Transform>(c));
        CHECK_CLOSE(r3.get<Transform>(a).pos.x, 1.0f, 1e-6f);
        CHECK_CLOSE(r3.get<Transform>(b).pos.x, 2.0f, 1e-6f);
        r3.destroy(a); // head remove (swap with tail)
        CHECK(!r3.has<Transform>(a));
        CHECK_CLOSE(r3.get<Transform>(b).pos.x, 2.0f, 1e-6f);
    }
    std::printf("ecs ok\n");
}

static void testEcsLimits() {
    // Stale handles must stay dead across generation wraps. alive_ holds the
    // full 12-bit gen; with a uint8_t store the slot wrapped back to the
    // original handle's gen after 256 recycles (stale handle "alive").
    {
        Registry r;
        Entity first = r.create();
        uint32_t idx = entityIndex(first);
        r.destroy(first);
        for (int i = 0; i < 255; ++i) { // 256 generation bumps total
            Entity e = r.create();
            CHECK(entityIndex(e) == idx);
            r.destroy(e);
        }
        CHECK(!r.alive(first)); // must not resurrect at the wrap point
        CHECK(!r.has<Transform>(first));
        Entity fresh = r.create();
        CHECK(r.alive(fresh) && entityIndex(fresh) == idx);
    }
    // Index space exhaustion: past 2^20 the masked index would alias slot 1.
    // create() must refuse with kNullEntity instead of issuing a duplicate.
    {
        Registry r;
        Entity last = kNullEntity;
        for (uint32_t i = 0; i < kMaxEntityIndex; ++i) last = r.create(); // indices 1..2^20-1
        CHECK(last != kNullEntity);
        CHECK(entityIndex(last) == kMaxEntityIndex);
        CHECK(r.create() == kNullEntity);
        CHECK(!r.alive(kNullEntity));
    }
    std::printf("ecs-limits ok\n");
}

static void testPhysics() {
    PhysicsWorld world;
    world.setGravity({0, -1000});

    Body ground;
    ground.type = BodyType::Static;
    ground.pos = {0, -50};
    ground.size = {1000, 100};
    uint32_t g = world.addBody(ground);

    Body ball;
    ball.type = BodyType::Dynamic;
    ball.pos = {0, 200};
    ball.size = {20, 20};
    ball.friction = 0.1f;
    uint32_t b = world.addBody(ball);
    (void)g;

    for (int i = 0; i < 300; ++i) world.step(1.0f / 60.0f);
    const Body* bb = world.get(b);
    CHECK(bb != nullptr);
    // ground top = 0; ball half = 10 -> rest y ~= 10
    CHECK_CLOSE(bb->pos.y, 10.0f, 2.0f);
    CHECK(bb->onGround);

    // raycast down from above should hit ball or ground
    uint32_t hit = 0;
    CHECK(world.raycast({0, 500}, {0, -500}, &hit, nullptr));
    CHECK(hit == b || hit == g);

    // query
    auto q = world.queryRect({-500, -100, 1000, 200});
    CHECK(q.size() >= 2);
    std::printf("physics ok (ball y=%.2f)\n", bb->pos.y);
}

static void testPhysicsExtra() {
    // Kinematic platform vs static wall: kinematic must be pushed out, not
    // tunnel through (old solver skipped the pair entirely).
    {
        PhysicsWorld w;
        w.setGravity({0, 0});
        Body wall;
        wall.type = BodyType::Static;
        wall.pos = {100, 0}; wall.size = {20, 400};
        w.addBody(wall);
        Body plat;
        plat.type = BodyType::Kinematic;
        plat.pos = {0, 0}; plat.size = {40, 40};
        plat.vel = {500, 0};
        uint32_t p = w.addBody(plat);
        for (int i = 0; i < 120; ++i) w.step(1.0f / 60.0f);
        const Body* pb = w.get(p);
        // wall left face = 90, plat half = 20 -> rest x <= 70
        CHECK(pb->pos.x <= 70.5f);
    }
    // Static sensor still reports contacts via callback.
    {
        PhysicsWorld w;
        w.setGravity({0, 0});
        int hits = 0;
        w.onContact([&](const Contact&) { ++hits; });
        Body sensor;
        sensor.type = BodyType::Static;
        sensor.sensor = true;
        sensor.pos = {0, 0}; sensor.size = {100, 100};
        w.addBody(sensor);
        Body dyn;
        dyn.type = BodyType::Dynamic;
        dyn.pos = {0, 0}; dyn.size = {10, 10};
        w.addBody(dyn);
        w.step(1.0f / 60.0f);
        CHECK(hits > 0);
        // ...and the dynamic body was NOT pushed out by the sensor
        CHECK_CLOSE(w.get(2)->pos.x, 0.0f, 1e-4f);
    }
    // Huge dt is clamped: no tunneling past the floor in one step.
    {
        PhysicsWorld w;
        w.setGravity({0, -1000});
        Body g;
        g.type = BodyType::Static; g.pos = {0, -50}; g.size = {1000, 100};
        w.addBody(g);
        Body b;
        b.type = BodyType::Dynamic; b.pos = {0, 200}; b.size = {20, 20};
        uint32_t id = w.addBody(b);
        w.step(1.0f); // would tunnel 500px without clamp+substeps
        CHECK(w.get(id)->pos.y > -100.0f);
    }
    // setCellSize(0) is ignored, world keeps working.
    {
        PhysicsWorld w;
        w.setCellSize(0);
        Body b;
        b.type = BodyType::Dynamic; b.pos = {0, 0}; b.size = {10, 10};
        uint32_t id = w.addBody(b);
        w.step(1.0f / 60.0f);
        CHECK(w.get(id) != nullptr);
    }
    std::printf("physics-extra ok\n");
}

static void testPhysicsSolver() {
    // Determinism: identical worlds stepped identically must match exactly
    // (the header promises a deterministic step).
    {
        auto build = [] {
            PhysicsWorld w;
            w.setGravity({0, -980});
            Body g;
            g.type = BodyType::Static; g.pos = {0, -50}; g.size = {800, 100};
            w.addBody(g);
            for (int i = 0; i < 8; ++i) {
                Body b;
                b.type = BodyType::Dynamic;
                b.pos = {(float)i * 24.0f - 84.0f, 120.0f + i * 40.0f};
                b.size = {20, 20};
                b.friction = 0.2f;
                b.restitution = 0.3f;
                w.addBody(b);
            }
            return w;
        };
        PhysicsWorld a = build(), b = build();
        for (int i = 0; i < 240; ++i) { a.step(1.0f / 60.0f); b.step(1.0f / 60.0f); }
        CHECK(a.bodyCount() == b.bodyCount());
        for (uint32_t id = 1; id <= a.bodyCount(); ++id) {
            const Body* pa = a.get(id);
            const Body* pb = b.get(id);
            CHECK(pa && pb);
            CHECK(pa->pos.x == pb->pos.x && pa->pos.y == pb->pos.y);
            CHECK(pa->vel.x == pb->vel.x && pa->vel.y == pb->vel.y);
        }
    }
    // Restitution: a bouncy ball must come back up off the floor.
    {
        PhysicsWorld w;
        w.setGravity({0, -1000});
        Body g;
        g.type = BodyType::Static; g.pos = {0, -50}; g.size = {1000, 100};
        w.addBody(g);
        Body b;
        b.type = BodyType::Dynamic; b.pos = {0, 150}; b.size = {20, 20};
        b.restitution = 0.8f;
        uint32_t id = w.addBody(b);
        bool bounced = false;
        float topAfter = 0;
        for (int i = 0; i < 180; ++i) {
            w.step(1.0f / 60.0f);
            const Body* pb = w.get(id);
            if (pb->vel.y > 100.0f) bounced = true; // moving up: it bounced
            if (bounced) topAfter = std::max(topAfter, pb->pos.y);
        }
        CHECK(bounced);
        CHECK(topAfter > 60.0f); // rebounded to a visible height
    }
    // Friction: a sliding box on a grippy floor slows down.
    {
        PhysicsWorld w;
        w.setGravity({0, -1000});
        Body g;
        g.type = BodyType::Static; g.pos = {0, -10}; g.size = {1000, 20};
        w.addBody(g);
        Body b;
        b.type = BodyType::Dynamic; b.pos = {-300, 10}; b.size = {20, 20};
        b.vel = {300, 0}; b.friction = 0.5f;
        uint32_t id = w.addBody(b);
        for (int i = 0; i < 120; ++i) w.step(1.0f / 60.0f);
        const Body* pb = w.get(id);
        CHECK(pb->onGround);
        CHECK(std::fabs(pb->vel.x) < 150.0f); // started at 300
    }
    // Stack: boxes resting on each other must not sink through.
    {
        PhysicsWorld w;
        w.setGravity({0, -1000});
        Body g;
        g.type = BodyType::Static; g.pos = {0, -25}; g.size = {400, 50};
        w.addBody(g);
        uint32_t ids[3];
        for (int i = 0; i < 3; ++i) {
            Body b;
            b.type = BodyType::Dynamic;
            b.pos = {0, 10.0f + i * 20.0f}; b.size = {20, 20};
            ids[i] = w.addBody(b);
        }
        for (int i = 0; i < 300; ++i) w.step(1.0f / 60.0f);
        CHECK(w.get(ids[0])->pos.y > 1.0f);  // floor top is y = 0
        CHECK(w.get(ids[1])->pos.y > 20.0f);
        CHECK(w.get(ids[2])->pos.y > 40.0f);
    }
    // raycast from INSIDE a body skips that body (no distance-0 hits).
    {
        PhysicsWorld w;
        w.setGravity({0, 0});
        Body big;
        big.type = BodyType::Static; big.pos = {0, 0}; big.size = {200, 200};
        w.addBody(big);
        Body small;
        small.type = BodyType::Static; small.pos = {300, 0}; small.size = {40, 40};
        uint32_t smallId = w.addBody(small);
        uint32_t hit = 0;
        Vec2 hp{};
        CHECK(w.raycast({0, 0}, {400, 0}, &hit, &hp));
        CHECK(hit == smallId); // the enclosing body must not win at t = 0
        CHECK_CLOSE(hp.x, 280.0f, 0.5f);
        PhysicsWorld w2; // only the enclosing body: nothing to hit
        Body only;
        only.type = BodyType::Static; only.pos = {0, 0}; only.size = {100, 100};
        w2.addBody(only);
        CHECK(!w2.raycast({0, 0}, {1000, 0}));
    }
    // queryRect uses the cached grid; must track moves, and get()/removeBody
    // index bookkeeping (swap-erase) must stay consistent.
    {
        PhysicsWorld w;
        w.setGravity({0, 0});
        Body a;
        a.type = BodyType::Dynamic; a.pos = {0, 0}; a.size = {10, 10};
        Body b;
        b.type = BodyType::Dynamic; b.pos = {10000, 0}; b.size = {10, 10};
        uint32_t ia = w.addBody(a), ib = w.addBody(b);
        auto q1 = w.queryRect({-50, -50, 100, 100});
        CHECK(q1.size() == 1 && q1[0] == ia);
        w.get(ib)->pos = {20, 0}; // direct write: query must see it
        auto q2 = w.queryRect({-50, -50, 100, 100});
        CHECK(q2.size() == 2);
        w.removeBody(ia);
        CHECK(w.get(ia) == nullptr);
        CHECK(w.get(ib) != nullptr && w.get(ib)->pos.x == 20.0f);
        auto q3 = w.queryRect({-50, -50, 100, 100});
        CHECK(q3.size() == 1 && q3[0] == ib);
    }
    std::printf("physics-solver ok\n");
}

static void testPhysicsFeatures() {
    // --- layer/mask filtering: a filtered pair never touches ---
    {
        PhysicsWorld w;
        w.setGravity({0, -1000});
        Body floor;
        floor.type = BodyType::Static;
        floor.pos = {0, -50}; floor.size = {400, 100};
        floor.layer = 0x2; floor.mask = 0x2;      // "world" layer only
        w.addBody(floor);
        Body ghost;
        ghost.type = BodyType::Dynamic;
        ghost.pos = {0, 120}; ghost.size = {20, 20};
        ghost.layer = 0x1; ghost.mask = 0x1;      // "player" layer only
        uint32_t gid = w.addBody(ghost);
        for (int i = 0; i < 120; ++i) w.step(1.0f / 60.0f);
        CHECK(w.get(gid)->pos.y < -200.0f);       // fell straight through
    }
    // ...and matching masks do collide.
    {
        PhysicsWorld w;
        w.setGravity({0, -1000});
        Body floor;
        floor.type = BodyType::Static;
        floor.pos = {0, -50}; floor.size = {400, 100};
        floor.layer = 0x2; floor.mask = 0x1 | 0x2;
        w.addBody(floor);
        Body player;
        player.type = BodyType::Dynamic;
        player.pos = {0, 120}; player.size = {20, 20};
        player.layer = 0x1; player.mask = 0x2;
        uint32_t pid = w.addBody(player);
        for (int i = 0; i < 180; ++i) w.step(1.0f / 60.0f);
        CHECK_CLOSE(w.get(pid)->pos.y, 10.0f, 2.0f);
        CHECK(w.get(pid)->onGround);
    }

    // --- one-way platform: lands from above, passes through from below ---
    {
        PhysicsWorld w;
        w.setGravity({0, -1000});
        Body plat;
        plat.type = BodyType::Static;
        plat.pos = {0, 100}; plat.size = {200, 16};
        plat.oneWay = true;
        w.addBody(plat);
        Body fromAbove;
        fromAbove.type = BodyType::Dynamic;
        fromAbove.pos = {0, 200}; fromAbove.size = {20, 20};
        uint32_t aid = w.addBody(fromAbove);
        for (int i = 0; i < 180; ++i) w.step(1.0f / 60.0f);
        CHECK(w.get(aid)->pos.y > 100.0f);        // resting on the platform

        PhysicsWorld w2;
        w2.setGravity({0, 0});
        Body plat2;
        plat2.type = BodyType::Static;
        plat2.pos = {0, 100}; plat2.size = {200, 16};
        plat2.oneWay = true;
        w2.addBody(plat2);
        Body jumper;
        jumper.type = BodyType::Dynamic;
        jumper.pos = {0, 40}; jumper.size = {20, 20};
        jumper.vel = {0, 400};
        uint32_t jid = w2.addBody(jumper);
        for (int i = 0; i < 60; ++i) w2.step(1.0f / 60.0f);
        CHECK(w2.get(jid)->pos.y > 200.0f);       // went up through it
    }

    // --- trigger enter/exit on sensors ---
    {
        PhysicsWorld w;
        w.setGravity({0, 0});
        int enters = 0, exits = 0;
        w.onTriggerEnter([&](const Contact& c) { CHECK(c.sensor); ++enters; });
        w.onTriggerExit([&](const Contact& c) { CHECK(c.sensor); ++exits; });
        Body sensor;
        sensor.type = BodyType::Static;
        sensor.sensor = true;
        sensor.pos = {0, 0}; sensor.size = {60, 60};
        w.addBody(sensor);
        Body mover;
        mover.type = BodyType::Kinematic;
        mover.pos = {-200, 0}; mover.size = {20, 20};
        mover.vel = {200, 0};
        w.addBody(mover);
        for (int i = 0; i < 180; ++i) w.step(1.0f / 60.0f);
        CHECK(enters == 1);
        CHECK(exits == 1);
    }

    // --- circle collider rests on the floor at its radius ---
    {
        PhysicsWorld w;
        w.setGravity({0, -1000});
        Body floor;
        floor.type = BodyType::Static;
        floor.pos = {0, -50}; floor.size = {400, 100};
        w.addBody(floor);
        Body ball;
        ball.type = BodyType::Dynamic;
        ball.shape = ShapeType::Circle;
        ball.radius = 12.0f;
        ball.pos = {0, 120};
        uint32_t bid = w.addBody(ball);
        for (int i = 0; i < 240; ++i) w.step(1.0f / 60.0f);
        CHECK_CLOSE(w.get(bid)->pos.y, 12.0f, 2.5f);
        CHECK(w.get(bid)->onGround);
        CHECK_CLOSE(w.get(bid)->bounds().w, 24.0f, 1e-4f);
    }

    // --- moving-platform carry: a rider inherits the platform's delta ---
    {
        PhysicsWorld w;
        w.setGravity({0, -1000});
        Body plat;
        plat.type = BodyType::Kinematic;
        plat.pos = {0, 0}; plat.size = {200, 16};
        plat.vel = {60, 0};
        uint32_t platId = w.addBody(plat);
        Body rider;
        rider.type = BodyType::Dynamic;
        rider.pos = {0, 40}; rider.size = {20, 20};
        uint32_t riderId = w.addBody(rider);
        for (int i = 0; i < 60; ++i) w.step(1.0f / 60.0f);
        const Body* r = w.get(riderId);
        const Body* p = w.get(platId);
        float lag = p->pos.x - r->pos.x;   // after landing + carry settles
        for (int i = 0; i < 60; ++i) w.step(1.0f / 60.0f);
        r = w.get(riderId);
        p = w.get(platId);
        CHECK(r->onGround);
        // The rider must not be left behind: the offset stays constant while
        // the platform keeps moving (it inherited every delta).
        CHECK_CLOSE(p->pos.x - r->pos.x, lag, 1.0f);
        CHECK_CLOSE(r->pos.x, p->pos.x, 20.0f);
        CHECK_CLOSE(r->pos.y, p->size.y * 0.5f + r->size.y * 0.5f, 2.0f);
    }

    // --- distance joint holds two bodies at their rest length ---
    {
        PhysicsWorld w;
        w.setGravity({0, 0});
        Body a;
        a.type = BodyType::Dynamic; a.pos = {0, 0}; a.size = {10, 10};
        Body b;
        b.type = BodyType::Dynamic; b.pos = {100, 0}; b.size = {10, 10};
        uint32_t ia = w.addBody(a), ib = w.addBody(b);
        Joint j;
        j.type = JointType::Distance;
        j.a = ia; j.b = ib;
        j.restLength = 50.0f;
        j.stiffness = 1.0f;
        uint32_t jid = w.addJoint(j);
        CHECK(jid != 0);
        CHECK(w.jointCount() == 1);
        for (int i = 0; i < 120; ++i) w.step(1.0f / 60.0f);
        float dist = (w.get(ib)->pos - w.get(ia)->pos).length();
        CHECK_CLOSE(dist, 50.0f, 8.0f);
        w.removeJoint(jid);
        CHECK(w.jointCount() == 0);
    }

    // --- CCD: speed-derived substeps stop a fast body at a thin wall ---
    // The substep count is capped at 16, so the test speed is chosen to be
    // within that budget (a 6000 px/s 4 px bullet needs ~100 substeps and is
    // documented to tunnel — that is the accepted arcade trade-off).
    {
        PhysicsWorld w;
        w.setGravity({0, 0});
        Body wall;
        wall.type = BodyType::Static;
        wall.pos = {0, 0}; wall.size = {4, 400};
        w.addBody(wall);
        Body bullet;
        bullet.type = BodyType::Dynamic;
        bullet.pos = {-100, 0}; bullet.size = {4, 4};
        bullet.vel = {900, 0};
        uint32_t bid = w.addBody(bullet);
        for (int i = 0; i < 20; ++i) w.step(1.0f / 60.0f);
        CHECK(w.get(bid)->pos.x < 0.0f);          // stopped before the wall
        CHECK(w.continuous());

        // The substep count grows with speed; a body slower than the CCD
        // budget is stopped, and the cap is the documented failure mode.
        PhysicsWorld slow;
        slow.setGravity({0, 0});
        Body wall2;
        wall2.type = BodyType::Static;
        wall2.pos = {0, 0}; wall2.size = {4, 400};
        slow.addBody(wall2);
        Body pebble;
        pebble.type = BodyType::Dynamic;
        pebble.pos = {-100, 0}; pebble.size = {4, 4};
        pebble.vel = {200, 0};
        uint32_t pid = slow.addBody(pebble);
        for (int i = 0; i < 40; ++i) slow.step(1.0f / 60.0f);
        CHECK(slow.get(pid)->pos.x < 0.0f);
    }
    std::printf("physics-features ok\n");
}

static void testScenePhysics() {
    // Collider is wired: a dynamic crate must land on a static floor.
    Scene scene;
    scene.physics().setGravity({0, -500});
    Entity floor = scene.create("floor");
    scene.registry().get<Transform>(floor).pos = {0, -50};
    scene.registry().emplace<Collider>(floor, Collider{{200, 50}});
    Entity crate = scene.create("crate");
    scene.registry().get<Transform>(crate).pos = {0, 120};
    Collider cc;
    cc.halfExtents = {10, 10};
    cc.friction = 0.5f;
    scene.registry().emplace<Collider>(crate, cc);
    scene.registry().emplace<Velocity>(crate);
    scene.update(1.0f / 60.0f); // first sync creates the bodies
    CHECK(scene.physics().bodyCount() == 2);
    for (int i = 0; i < 300; ++i) scene.update(1.0f / 60.0f);
    float y = scene.registry().get<Transform>(crate).pos.y;
    CHECK_CLOSE(y, 10.0f, 2.0f); // floor top = 0, crate half = 10
    CHECK(scene.registry().get<Velocity>(crate).v.y > -50.0f); // settled-ish
    // Destroying the entity retracts its body.
    scene.destroy(crate);
    CHECK(scene.physics().bodyCount() == 1);
    scene.update(1.0f / 60.0f);
    CHECK(scene.physics().bodyCount() == 1);
    // clear() wipes both sides.
    scene.clear();
    CHECK(scene.physics().bodyCount() == 0);
    std::printf("scene-physics ok\n");
}

static void testCamera() {
    Camera2D cam;
    cam.position = {100, -50};
    cam.zoom = 2.5f;
    cam.rotation = 0.7f;
    const int W = 1280, H = 720;
    // Round-trip at zoom != 1, rot != 0 (old viewProj disagreed with pick)
    Vec2 pts[] = {{0, 0}, {100, -50}, {-300, 200}, {1000, -1000}};
    for (Vec2 p : pts) {
        Vec2 s = cam.worldToScreen(p, W, H);
        Vec2 back = cam.screenToWorld(s, W, H);
        CHECK_CLOSE(back.x, p.x, 0.05f);
        CHECK_CLOSE(back.y, p.y, 0.05f);
    }
    // Camera center always maps to viewport center (= NDC 0,0)
    Vec2 c = cam.worldToScreen(cam.position, W, H);
    CHECK_CLOSE(c.x, W * 0.5f, 0.05f);
    CHECK_CLOSE(c.y, H * 0.5f, 0.05f);
    Mat4 vp = cam.viewProj(W, H);
    Vec2 ndc = vp.transformPoint(cam.position);
    // proj maps centered-origin to clip; center must be ~origin pre-proj...
    // verify via clip: transformPoint applies view+proj; center -> (0,0)
    CHECK_CLOSE(ndc.x, 0.0f, 0.01f);
    CHECK_CLOSE(ndc.y, 0.0f, 0.01f);
    std::printf("camera ok\n");
}

static void testInput() {
    Input in;
    in.beginFrame();
    // Out-of-range buttons must be safe no-ops / false (old code OOB-read)
    CHECK(!in.mouseDown((MouseButton)99));
    CHECK(!in.mousePressed((MouseButton)99));
    CHECK(!in.mouseReleased((MouseButton)99));
    CHECK(!in.padDown(0, (GamepadButton)99));
    CHECK(!in.padDown(99, GamepadButton::A));
    CHECK(!in.padPressed(0, (GamepadButton)99));
    CHECK(!in.padReleased(0, (GamepadButton)99));
    in.setMouseButton((MouseButton)99, true);   // must not corrupt
    in.setGamepadButton(0, (GamepadButton)99, true);
    CHECK(!in.mouseDown(MouseButton::Left));
    // Normal path still works
    in.setKey(Key::Space, true);
    CHECK(in.isDown(Key::Space));
    in.beginFrame();
    CHECK(in.isDown(Key::Space) && !in.pressed(Key::Space));
    in.setKey(Key::Space, false);
    CHECK(in.released(Key::Space));
    // actionReleased covers gamepad (old code only checked keys+mouse)
    in.bindPad("jump", GamepadButton::A);
    in.connectGamepad(0);
    in.beginFrame();
    in.setGamepadButton(0, GamepadButton::A, true);
    CHECK(in.actionPressed("jump"));
    in.beginFrame();
    in.setGamepadButton(0, GamepadButton::A, false);
    CHECK(in.actionReleased("jump"));
    std::printf("input ok\n");
}

static void testInputSurface() {
    Input in;
    in.connectGamepad(0);
    // Every enum value must round-trip (kPadBtns was 14: DpadRight was
    // silently rejected by setGamepadButton and always read false).
    for (int i = 0; i < (int)GamepadButton::Count; ++i) {
        GamepadButton b = (GamepadButton)i;
        in.beginFrame();
        in.setGamepadButton(0, b, true);
        CHECK(in.padDown(0, b));
        CHECK(in.padPressed(0, b));
        CHECK(in.anyPadDown(b));
        in.setGamepadButton(0, b, false);
        CHECK(!in.padDown(0, b));
        CHECK(in.padReleased(0, b));
    }
    // Sub-frame taps: down AND up between two beginFrame() calls must be
    // visible to both pressed() and released() (old code missed them).
    in.beginFrame();
    in.setKey(Key::J, true);
    in.setKey(Key::J, false);
    CHECK(in.pressed(Key::J) && in.released(Key::J) && !in.isDown(Key::J));
    in.beginFrame();
    CHECK(!in.pressed(Key::J) && !in.released(Key::J));
    // ...including through action mapping.
    in.bindKey("tap", Key::K);
    in.beginFrame();
    in.setKey(Key::K, true);
    in.setKey(Key::K, false);
    CHECK(in.actionPressed("tap") && in.actionReleased("tap"));
    // Mouse tap in one frame.
    in.beginFrame();
    in.setMouseButton(MouseButton::Left, true);
    in.setMouseButton(MouseButton::Left, false);
    CHECK(in.mousePressed(MouseButton::Left) && in.mouseReleased(MouseButton::Left));
    // A release with no preceding press must not report a press.
    in.beginFrame();
    in.setGamepadButton(0, GamepadButton::A, false);
    CHECK(!in.padPressed(0, GamepadButton::A));
    // Repeated down events with no release are a single press.
    in.beginFrame();
    in.setKey(Key::M, true);
    in.setKey(Key::M, true);
    CHECK(in.pressed(Key::M));
    in.beginFrame();
    CHECK(in.isDown(Key::M) && !in.pressed(Key::M)); // held, not re-pressed
    std::printf("input-surface ok\n");
}

// Regression: input edges must survive frames that run zero fixed steps.
// runFixed pumps events once per rendered frame, but the stepper may run
// 0..maxSteps updates; the old code committed input at the top of every
// rendered frame, so a press latched on a 0-step frame was cleared before
// any update could see it (needed 2-3 presses to register at 144Hz vsync
// against a 60Hz step). The loop now commits (endFrame) after each update
// actually observes the window, delivering every edge exactly once.
static void testInputStepWindow() {
    Input in;
    // Frame 1: key pressed, but the stepper ran 0 steps (no commit yet).
    in.setKey(Key::P, true);
    CHECK(in.pressed(Key::P));
    // Frame 2: one step runs; the update must see the press exactly once.
    int seen = 0;
    for (int step = 0; step < 1; ++step) {
        if (in.pressed(Key::P)) ++seen;
        in.endFrame(); // commit after each observed step (runFixed)
    }
    CHECK(seen == 1);
    CHECK(in.isDown(Key::P));     // key still held...
    CHECK(!in.pressed(Key::P));   // ...but the edge was consumed

    // Two steps in one frame observe the same edge only once.
    in.setKey(Key::R, true);
    seen = 0;
    for (int step = 0; step < 2; ++step) {
        if (in.pressed(Key::R)) ++seen;
        in.endFrame();
    }
    CHECK(seen == 1);

    // Release edges get the same treatment.
    in.setKey(Key::R, false);
    CHECK(in.released(Key::R));   // latches on a 0-step frame...
    seen = 0;
    for (int step = 0; step < 1; ++step) {
        if (in.released(Key::R)) ++seen;
        in.endFrame();
    }
    CHECK(seen == 1);
    CHECK(!in.isDown(Key::R) && !in.released(Key::R));

    // A tap (down AND up) inside a 0-step frame is seen by the next step.
    in.setKey(Key::J, true);
    in.setKey(Key::J, false);
    bool tap = false;
    for (int step = 0; step < 1; ++step) {
        tap = in.pressed(Key::J) && in.released(Key::J) && !in.isDown(Key::J);
        in.endFrame();
    }
    CHECK(tap);
    CHECK(!in.pressed(Key::J) && !in.released(Key::J));
    std::printf("input-step-window ok\n");
}

static void testTilemap() {
    Tilemap map;
    map.create(4, 4, 16, 16);
    map.setTile(0, 1, 1, 5);
    CHECK(map.tile(0, 1, 1) == 5);
    CHECK(map.tile(0, 0, 0) == 0);

    map.setSolid(5, true);
    CHECK(map.isSolid(5));
    CHECK(!map.isSolid(3));
    auto solids = map.solidsInRect({0, 0, 64, 64});
    CHECK(solids.size() == 1);

    // CSV loader (row 0 = bottom in Y-up)
    Tilemap m2;
    CHECK(m2.loadCsv("1,0,0,0, 0,2,0,0, 0,0,0,0, 0,0,0,3", 4, 4, 8, 8));
    CHECK(m2.tile(0, 0, 0) == 1);
    CHECK(m2.tile(0, 1, 1) == 2);
    CHECK(m2.tile(0, 3, 3) == 3);

    // Layer visibility is a render concern: an invisible layer's solids must
    // still be returned (hiding a collision layer must not remove collision).
    {
        std::filesystem::path p =
            std::filesystem::temp_directory_path() / "forge_test_invisible.json";
        { std::ofstream f(p); f << R"JSON({
            "width": 2, "height": 1, "tilewidth": 16, "tileheight": 16,
            "tilesets": [{"firstgid": 1, "image": "ts.png", "columns": 2, "tilecount": 2}],
            "layers": [{"type": "tilelayer", "name": "solid", "width": 2, "height": 1,
                        "visible": false, "data": [1, 0]}]
        })JSON"; }
        Tilemap mi;
        CHECK(mi.loadTiledJson(p.string()));
        mi.setSolid(1, true);
        CHECK(mi.solidsInRect({0, 0, 16, 16}).size() == 1);
        std::filesystem::remove(p);
    }
    // --- slopes ---
    {
        Tilemap sm;
        sm.create(2, 1, 16, 16);
        sm.setTile(0, 0, 0, 7);
        sm.setSolid(7, true);
        sm.setSlope(7, 0.0f, 1.0f); // 45 degrees, low on the left
        CHECK(sm.isSlope(7));
        CHECK(sm.isSolid(7)); // setSlope implies solid
        CHECK_CLOSE(sm.slope(7).left, 0.0f, 1e-6f);
        CHECK_CLOSE(sm.slope(7).right, 1.0f, 1e-6f);
        CHECK_CLOSE(sm.slopeSurfaceHeight(7, 0.0f), 0.0f, 1e-5f);
        CHECK_CLOSE(sm.slopeSurfaceHeight(7, 0.5f), 8.0f, 1e-5f);
        CHECK_CLOSE(sm.slopeSurfaceHeight(7, 1.0f), 16.0f, 1e-5f);
        CHECK_CLOSE(sm.surfaceYAt(0, 0, 0, 8.0f), 8.0f, 1e-5f);
        // A box at the left edge rests on the low part of the ramp; the same
        // box on the right is pushed up further by the higher surface.
        Rect left{2, 4, 8, 8};
        Vec2 push;
        CHECK(sm.collideRect(left, &push));
        // collideRect takes the rect by value; callers apply `push` themselves.
        CHECK_CLOSE(push.y, 2.0f, 0.01f); // surface y=6, box bottom was 4
        Rect right{10, 4, 4, 4};
        CHECK(sm.collideRect(right, &push));
        CHECK(push.y > 2.0f);
        sm.clearSlope(7);
        CHECK(!sm.isSlope(7));
    }
    std::printf("tilemap ok\n");
}

static void testTilemapJson() {
    // Tiled export: top-down rows, firstgid offset, flip-bit flagged tile.
    // Forge flips to Y-up on load and remaps firstgid.
    const char* json = R"JSON({
        "width": 3, "height": 2, "tilewidth": 16, "tileheight": 16,
        "tilesets": [{"firstgid": 5, "image": "ts.png", "columns": 4, "tilecount": 8}],
        "layers": [{"type": "tilelayer", "name": "g", "width": 3, "height": 2,
                    "visible": true, "opacity": 1.0,
                    "data": [0, 6, 0, 7, 0, 2147483657]}]
    })JSON";
    std::filesystem::path tmp = std::filesystem::temp_directory_path() / "forge_test_map.json";
    { std::ofstream f(tmp); f << json; }
    Tilemap m;
    CHECK(m.loadTiledJson(tmp.string()));
    CHECK(m.width() == 3 && m.height() == 2);
    CHECK(m.tilesetPath() == "ts.png");
    // Tiled row 0 (top): [0,6,0] -> Forge y=1; row 1: [7,0,flip|9] -> y=0.
    // firstgid 5: gid 6 -> local 2, gid 7 -> local 3, gid 9 -> local 5.
    CHECK(m.tile(0, 1, 1) == 2);
    CHECK(m.tile(0, 0, 0) == 3);
    CHECK(m.tile(0, 2, 0) == 5); // 0x80000009 masked to gid 9
    CHECK(m.tile(0, 0, 1) == 0);
    // Missing keys must fail cleanly, not segfault (old code deref'd null).
    std::filesystem::path bad = std::filesystem::temp_directory_path() / "forge_test_bad.json";
    { std::ofstream f(bad); f << "{\"layers\": []}"; }
    Tilemap m2;
    CHECK(!m2.loadTiledJson(bad.string()));
    CHECK(!m2.loadTiledJson((std::filesystem::temp_directory_path() / "forge_nope.json").string()));
    // Tileset tile size different from the map tile size would break tileUv():
    // must be rejected at load instead of rendering garbage.
    {
        std::filesystem::path mismatch =
            std::filesystem::temp_directory_path() / "forge_test_mismatch.json";
        { std::ofstream f(mismatch); f << R"JSON({
            "width": 2, "height": 1, "tilewidth": 16, "tileheight": 16,
            "tilesets": [{"firstgid": 1, "image": "ts.png",
                          "tilewidth": 32, "tileheight": 32, "columns": 2, "tilecount": 2}],
            "layers": [{"type": "tilelayer", "name": "g", "width": 2, "height": 1,
                        "data": [1, 0]}]
        })JSON"; }
        Tilemap m3;
        CHECK(!m3.loadTiledJson(mismatch.string()));
        std::filesystem::remove(mismatch);
    }
    std::filesystem::remove(tmp);
    std::filesystem::remove(bad);

    // --- multi-tileset, flip flags, object layers, parallax ---
    {
        std::filesystem::path p =
            std::filesystem::temp_directory_path() / "forge_test_multi.json";
        { std::ofstream f(p); f << R"JSON({
            "width": 3, "height": 2, "tilewidth": 16, "tileheight": 16,
            "tilesets": [
                {"firstgid": 1, "image": "a.png", "columns": 4, "tilecount": 8},
                {"firstgid": 101, "image": "b.png", "columns": 2, "tilecount": 4}
            ],
            "layers": [
                {"type": "tilelayer", "name": "bg", "width": 3, "height": 2,
                 "offsetx": 4, "offsety": -2, "parallaxx": 0.5, "parallaxy": 0.25,
                 "data": [1, 0, 0, 0, 0, 0]},
                {"type": "tilelayer", "name": "fg", "width": 3, "height": 2,
                 "visible": false, "opacity": 0.5,
                 "data": [0, 0, 0, 0, 2147483753, 0]},
                {"type": "objectgroup", "name": "spawns", "objects": [
                    {"name": "start", "type": "spawn", "x": 16, "y": 0,
                     "width": 16, "height": 16},
                    {"name": "goal", "type": "spawn", "x": 32, "y": 16, "point": true}
                ]}
            ]
        })JSON"; }
        Tilemap mm;
        CHECK(mm.loadTiledJson(p.string()));
        CHECK(mm.width() == 3 && mm.height() == 2);
        CHECK(mm.tilesets().size() == 2);
        CHECK(mm.layerCount() == 2);
        // Tiled's first data row is the top row -> Y-up y = height-1.
        CHECK(mm.tile(0, 0, 1) == 1);
        CHECK(mm.tilesetIndex(0, 0, 1) == 0);
        CHECK_CLOSE(mm.layer(0).offset.x, 4.0f, 1e-6f);
        CHECK_CLOSE(mm.layer(0).parallax.x, 0.5f, 1e-6f);
        CHECK(mm.layer(1).visible == false);
        CHECK_CLOSE(mm.layer(1).opacity, 0.5f, 1e-6f);
        // 0x80000069 = flipH + gid 105 (firstgid 101 -> local 5).
        CHECK(mm.tile(1, 1, 0) == 5);
        CHECK(mm.tilesetIndex(1, 1, 0) == 1);
        CHECK(mm.tileFlags(1, 1, 0) == 1);
        // Objects: top-down y converted to Y-up (map is 32 px tall).
        CHECK(mm.objects().size() == 2);
        CHECK(mm.objects()[0].name == "start");
        CHECK(mm.objects()[0].type == "spawn");
        CHECK_CLOSE(mm.objects()[0].rect.x, 16.0f, 1e-5f);
        CHECK_CLOSE(mm.objects()[0].rect.y, 16.0f, 1e-5f); // 32 - (0 + 16)
        CHECK(mm.objects()[1].point);
        // Solids are per-id across tilesets.
        mm.setSolid(5, true);
        CHECK(mm.solidsInRect({16, 16, 16, 16}).size() == 1);
        std::filesystem::remove(p);
    }

    // --- infinite map chunks + origin offset ---
    {
        std::filesystem::path p =
            std::filesystem::temp_directory_path() / "forge_test_infinite.json";
        { std::ofstream f(p); f << R"JSON({
            "infinite": true, "width": 0, "height": 0,
            "tilewidth": 16, "tileheight": 16,
            "tilesets": [{"firstgid": 1, "image": "a.png", "columns": 1, "tilecount": 1}],
            "layers": [{"type": "tilelayer", "name": "g", "width": 0, "height": 0,
                "chunks": [
                    {"x": -16, "y": 0, "width": 16, "height": 16,
                     "data": [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
                              0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                              0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                              0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                              0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                              0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                              0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                              0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                              0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                              0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                              0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                              0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                              0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                              0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                              0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                              0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]},
                    {"x": 0, "y": 0, "width": 16, "height": 16,
                     "data": [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                              0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                              0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                              0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                              0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                              0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                              0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                              0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                              0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                              0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                              0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                              0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                              0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                              0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                              0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                              0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1]}
                ]}]
        })JSON"; }
        Tilemap im;
        CHECK(im.loadTiledJson(p.string()));
        // Two 16-wide chunks, Tiled y 0..15 -> 16 rows, 32 columns.
        CHECK(im.width() == 32 && im.height() == 16);
        CHECK_CLOSE(im.originOffset().x, -16.0f * 16.0f, 1e-4f);
        // Tiled chunk row 0 is the top row -> Y-up row 15.
        CHECK(im.tile(0, 15, 15) == 1);
        // Second chunk's last data row is Tiled's bottom row (y=15) -> dy 0;
        // its last column lands at dx 31, world x = -256 + 31*16 = 240.
        CHECK(im.tile(0, 31, 0) == 1);
        im.setSolid(1, true);
        CHECK(im.solidsInRect({240.0f, 0.0f, 16.0f, 16.0f}).size() == 1);
        std::filesystem::remove(p);
    }

    // --- autotile ---
    {
        Tilemap am;
        am.create(3, 3, 16, 16);
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 3; ++x) am.setTile(0, x, y, 1);
        auto rule = Tilemap::buildAutoRule(1, 2, 3, 4);
        CHECK(rule.size() == 256);
        CHECK(rule[0] == 1);          // isolated -> base
        CHECK(rule[0xFF] == 4);       // full ring -> inner
        am.setAutoRule(rule);
        am.applyAutotile(0);
        CHECK(am.tile(0, 1, 1) == 4); // center is surrounded
        CHECK(am.tile(0, 0, 0) == 2 || am.tile(0, 0, 0) == 3);
    }
    std::printf("tilemap-json ok\n");
}

static void testAnim() {
    AnimClip clip = AnimClip::gridRow("run", 0, 4, 16, 16, 10.0f, true);
    CHECK(clip.frames.size() == 4);
    CHECK_CLOSE(clip.duration(), 0.4f, 1e-5f);

    Animator a;
    a.addClip(clip);
    CHECK(a.play("run"));
    a.update(0.15f); // 1.5 frames
    CHECK(a.frameIndex() == 1);
    a.update(1.0f); // loops
    CHECK(a.playing());

    AnimClip once = AnimClip::gridRow("hit", 1, 2, 16, 16, 10.0f, false);
    a.addClip(once);
    a.play("hit");
    a.update(5.0f);
    CHECK(a.finished());

    Tween t;
    t.from = 0; t.to = 10; t.duration = 1.0f;
    t.ease = easing::linear;
    t.update(0.5f);
    CHECK_CLOSE(t.value(), 5.0f, 1e-4f);

    Tweens ts;
    bool done = false;
    Tween t2;
    t2.from = 0; t2.to = 1; t2.duration = 0.5f;
    t2.onDone = [&] { done = true; };
    ts.add(t2);
    ts.update(1.0f);
    CHECK(done && ts.empty());

    // Loop mode wraps; PingPong swaps direction (old update skipped items)
    {
        Tweens loop;
        float seen = -1;
        Tween tl;
        tl.from = 0; tl.to = 4; tl.duration = 1.0f;
        tl.mode = TweenMode::Loop; tl.ease = easing::linear;
        tl.onUpdate = [&](float v) { seen = v; };
        loop.add(tl);
        loop.update(1.5f); // wraps: 0.5 into second lap
        CHECK_CLOSE(seen, 2.0f, 1e-4f);
        CHECK(!loop.empty());
    }
    {
        Tweens pp;
        float seen = -1;
        Tween tp;
        tp.from = 0; tp.to = 10; tp.duration = 1.0f;
        tp.mode = TweenMode::PingPong; tp.ease = easing::linear;
        tp.onUpdate = [&](float v) { seen = v; };
        pp.add(tp);
        pp.update(1.0f); // end of first leg -> swaps to 10->0
        pp.update(0.5f); // halfway back
        CHECK_CLOSE(seen, 5.0f, 1e-4f);
    }
    {
        // A long frame must carry its overflow across legs (old code swapped
        // once and clamped the remainder: 2.5 legs would report the value at
        // the end of leg 2 instead of 0.5 into leg 3).
        Tweens pp;
        float seen = -1;
        Tween tl;
        tl.from = 0; tl.to = 10; tl.duration = 1.0f;
        tl.mode = TweenMode::PingPong; tl.ease = easing::linear;
        tl.onUpdate = [&](float v) { seen = v; };
        pp.add(tl);
        pp.update(2.5f); // 0.5 into the third leg (0 -> 10)
        CHECK_CLOSE(seen, 5.0f, 1e-4f);
    }
    // Per-clip frame events (old map was global across clips)
    {
        Animator an;
        an.addClip(AnimClip::gridRow("a", 0, 3, 8, 8, 10.0f, true));
        an.addClip(AnimClip::gridRow("b", 1, 3, 8, 8, 10.0f, true));
        int fireA = 0, fireB = 0;
        an.setFrameEvent("a", 1, [&] { ++fireA; });
        an.setFrameEvent("b", 1, [&] { ++fireB; });
        an.play("a");
        an.update(0.15f); // frame 1 of clip a
        CHECK(fireA == 1 && fireB == 0);
        an.play("b");
        an.update(0.15f); // frame 1 of clip b
        CHECK(fireA == 1 && fireB == 1);
    }
    std::printf("anim ok\n");
}

static std::vector<uint8_t> testWav(float freq, float dur) {
    const int rate = 22050;
    int frames = (int)(dur * rate);
    std::vector<int16_t> pcm(frames);
    for (int i = 0; i < frames; ++i) {
        float t = (float)i / rate;
        float env = 1.0f - (float)i / frames;
        pcm[i] = (int16_t)(std::sin(6.2831853f * freq * t) * env * 20000);
    }
    std::vector<uint8_t> wav(44 + frames * 2);
    auto w32 = [&](int o, uint32_t v) { std::memcpy(&wav[o], &v, 4); };
    auto w16 = [&](int o, uint16_t v) { std::memcpy(&wav[o], &v, 2); };
    std::memcpy(&wav[0], "RIFF", 4);
    w32(4, 36 + frames * 2);
    std::memcpy(&wav[8], "WAVEfmt ", 8);
    w32(16, 16); w16(20, 1); w16(22, 1);
    w32(24, rate); w32(28, rate * 2); w16(32, 2); w16(34, 16);
    std::memcpy(&wav[36], "data", 4);
    w32(40, frames * 2);
    std::memcpy(&wav[44], pcm.data(), frames * 2);
    return wav;
}

static void testAudioHeadless() {
    // Decode + voice management need no playback device (headless-safe).
    AudioEngine au;
    auto wav = testWav(440, 0.2f);
    SoundHandle h = au.loadMemory(wav.data(), wav.size());
    CHECK(h.valid());
    CHECK(!au.loadMemory(nullptr, 0).valid());
    uint32_t v = au.play(h, 0.8f, 0.0f, 1.0f, false);
    CHECK(v != 0);
    CHECK(au.voicePlaying(v));
    CHECK(au.voiceCount() == 1);
    au.setVoiceVolume(v, 0.5f);
    au.stop(v);
    CHECK(!au.voicePlaying(v));
    CHECK(au.play({}, 1.0f) == 0); // invalid handle
    au.unload(h);
    CHECK(au.play(h, 1.0f) == 0); // unloaded
    std::printf("audio-headless ok\n");
}

static void testMathExtra() {
    // hash01 must be deterministic and in range (was signed-overflow UB)
    float h1 = hash01(-5, 7), h2 = hash01(-5, 7);
    CHECK(h1 == h2 && h1 >= 0.0f && h1 <= 1.0f);
    // pack rounds: 0.5 -> 128, not 127
    Color half{0.5f, 0.5f, 0.5f, 1.0f};
    CHECK(((half.pack() & 0xFF)) == 128);
    // rangeInt guards + stays in bounds over many draws
    Random rng{99};
    for (int i = 0; i < 200; ++i) {
        int v = rng.rangeInt(3, 9);
        CHECK(v >= 3 && v <= 9);
    }
    CHECK(rng.rangeInt(9, 3) >= 3); // swapped args safe
    std::printf("math-extra ok\n");
}

static void testScene() {
    Scene scene;
    Entity e = scene.create("hero");
    CHECK(scene.registry().alive(e));
    CHECK(scene.findByTag("hero") == e);
    CHECK(scene.findByTag("missing") == kNullEntity);
    // Script that destroys its own entity mid-update must not corrupt update
    Script suicide;
    suicide.onUpdate = [](Entity self, Registry& reg, float) { reg.destroy(self); };
    scene.registry().emplace<Script>(e, std::move(suicide));
    scene.update(0.016f); // would UAF inside view before fix
    CHECK(!scene.registry().alive(e));
    scene.update(0.016f); // no scripts left, still safe
    std::printf("scene ok\n");
}

static void testJson() {
    // Round-trip: ordered objects, escapes, nested arrays, UTF-16 escapes.
    const char* src = R"({"b":1,"a":[true,false,null,"x\ny"],"u":"\u00e9\ud83d\ude00"})";
    json::ParseError err;
    json::Value v = json::parse(src, &err);
    CHECK(err.ok());
    CHECK(v.isObject());
    CHECK(v.size() == 3);
    CHECK(v.items()[0].first == "b"); // insertion order preserved
    CHECK(v["a"].isArray() && v["a"].size() == 4);
    CHECK(v["a"].at(0).asBool());
    CHECK(v["a"].at(1).isBool() && !v["a"].at(1).asBool());
    CHECK(v["a"].at(2).isNull());
    CHECK(v["a"].at(3).asString() == "x\ny");
    // U+00E9 + U+1F600
    CHECK(v["u"].asString() == "\xC3\xA9\xF0\x9F\x98\x80");

    std::string out = json::write(v, true);
    json::Value again = json::parse(out, &err);
    CHECK(err.ok());
    CHECK(again["b"].asInt() == 1);
    CHECK(again["u"].asString() == v["u"].asString());

    // Error reporting carries a line.
    json::Value bad = json::parse("{\n  \"a\": }", &err);
    CHECK(!err.ok());
    CHECK(err.line == 2);
    CHECK(bad.isNull());

    // Non-finite numbers are written as 0 (valid JSON).
    json::Value n = json::Value::array();
    n.push(json::Value(std::nan("")));
    CHECK(json::write(n) == "[0]");
    std::printf("json ok\n");
}

static void testScript() {
    using namespace forge::script;
    ScriptVM vm;
    std::string printed;
    vm.printTo([&](const std::string& s) { printed += s; });

    CHECK(vm.loadSource(
        "var total = 0;\n"
        "fn add(a, b) { total = total + a + b; return total; }\n"
        "fn fib(n) { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); }\n"
        "fn loop() { var i = 0; var s = 0; while (i < 10) { s = s + i; i = i + 1; } return s; }\n"
        "fn logic(a, b) { return (a > 1 && b < 5) || a == 0; }\n"
        "fn text() { return \"a\" + 1 + \"b\"; }\n",
        "test.fgs"));
    CHECK(vm.error().ok());

    CHECK(vm.call("add", args(2.0, 3.0)).number() == 5.0);
    CHECK(vm.call("add", args(1.0, 1.0)).number() == 7.0);
    CHECK(vm.call("fib", args(10)).number() == 55.0);
    CHECK(vm.call("loop").number() == 45.0);
    CHECK(vm.call("logic", args(2.0, 1.0)).boolean());
    CHECK(!vm.call("logic", args(1.0, 9.0)).boolean());
    CHECK(vm.call("text").string() == "a1b");

    // Globals are visible to scripts and written back through tables.
    vm.setGlobal("speed", 4.0);
    CHECK(vm.loadSource("fn bump() { speed = speed * 2; return speed; }", "globals"));
    CHECK(vm.call("bump").number() == 8.0);
    CHECK(vm.getGlobal("speed").number() == 8.0);

    Value self = Value::table();
    self.set("x", 10.0);
    vm.setGlobal("self", self);
    CHECK(vm.loadSource("fn move(dx) { self.x = self.x + dx; return self.x; }", "member"));
    CHECK(vm.call("move", args(5.0)).number() == 15.0);
    CHECK(vm.getGlobal("self").get("x").number() == 15.0);

    // Host bindings.
    int spawned = 0;
    vm.bind("spawn", [&](const std::vector<Value>& a) {
        spawned += (int)a.size();
        return Value(true);
    });
    CHECK(vm.loadSource("fn go() { return spawn(1, 2, 3); }", "host"));
    CHECK(vm.call("go").boolean());
    CHECK(spawned == 3);

    // Errors are reported, never thrown, and the VM stays usable.
    CHECK(!vm.loadSource("fn bad() { return 1 + ; }", "parse"));
    CHECK(!vm.error().ok());
    CHECK(!vm.loadSource("fn x() { return 1; }\nfn x() {", "parse2"));
    CHECK(!vm.error().ok());
    CHECK(vm.loadSource("fn boom() { return 1 / 0; }", "runtime"));
    CHECK(vm.error().ok());
    vm.call("boom");
    CHECK(!vm.error().ok());
    CHECK(vm.error().message.find("zero") != std::string::npos);
    // Missing function.
    vm.call("nope");
    CHECK(!vm.error().ok());
    // Step budget stops infinite loops.
    CHECK(vm.loadSource("fn spin() { while (true) { } return 0; }", "spin"));
    vm.setStepBudget(1000);
    vm.call("spin");
    CHECK(!vm.error().ok());
    CHECK(vm.error().message.find("budget") != std::string::npos);
    vm.setStepBudget(1000000);

    // Functions list + hasFunction.
    CHECK(vm.hasFunction("spin"));
    CHECK(!vm.functions().empty());
    std::printf("script ok\n");
}

static void testAtlas() {
    AtlasPacker packer(128, 128, 2);
    int a = packer.add(64, 32, "a");
    int b = packer.add(32, 32, "b");
    int c = packer.add(32, 64, "c");
    CHECK(a == 0 && b == 1 && c == 2);
    CHECK(packer.pack());
    CHECK(packer.count() == 3);
    CHECK(packer.indexOf("b") == 1);
    CHECK(packer.name(0) == "a");
    CHECK(packer.rect(0).w == 64 && packer.rect(0).h == 32);
    CHECK(packer.occupancy() > 0.0f && packer.occupancy() <= 1.0f);
    CHECK(packer.pixels().size() == 128u * 128u * 4u);
    // No overlap between packed rects.
    for (int i = 0; i < 3; ++i)
        for (int j = i + 1; j < 3; ++j)
            CHECK(!packer.rect(i).overlaps(packer.rect(j)));

    // A too-large entry is rejected outright (add returns -1)...
    AtlasPacker small(64, 64);
    CHECK(small.add(128, 16) == -1);
    // ...and a layout that cannot fit every entry makes pack() fail rather
    // than overlapping.
    CHECK(small.add(48, 48) >= 0);
    CHECK(small.add(48, 48) >= 0);
    CHECK(!small.pack());

    // blit copies an RGBA source into the slot.
    std::vector<uint8_t> src(4u * 4u * 4u, 200);
    CHECK(packer.blit(0, src.data(), 4, 4));
    Rect r = packer.rect(0);
    size_t off = ((size_t)r.y * 128 + (size_t)r.x) * 4;
    CHECK(packer.pixels()[off] == 200);
    std::printf("atlas ok\n");
}

static void testAssetResolve() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "forge_test_assets";
    fs::remove_all(dir);
    fs::create_directories(dir / "assets");
    { std::ofstream f(dir / "assets" / "hero.png"); f << "not a png"; }

    AssetManager assets;
    CHECK(assets.searchPaths().empty());
    // No search path: an existing relative path resolves to itself.
    fs::path cwd = fs::current_path();
    fs::current_path(dir);
    CHECK(assets.resolve("assets/hero.png") == "assets/hero.png");
    // A path that only exists under the search dir resolves there.
    assets.addSearchPath("assets/");
    CHECK(assets.resolve("hero.png") == "assets/hero.png");
    CHECK(assets.searchPaths().size() == 1);
    // Unknown paths fall through unchanged (the loader then reports the error).
    CHECK(assets.resolve("missing.png") == "missing.png");
    // Absolute paths bypass the search list.
    std::string abs = (dir / "assets" / "hero.png").string();
    CHECK(assets.resolve(abs) == abs);
    // An empty path stays empty (no accidental directory lookup).
    CHECK(assets.resolve("").empty());
    assets.clearSearchPaths();
    CHECK(assets.searchPaths().empty());
    fs::current_path(cwd);

    // A missing shader reports failure instead of throwing (no GL needed:
    // the file check happens before compile).
    CHECK(assets.loadShader("nope.vert", "nope.frag") == nullptr);

    fs::remove_all(dir);
    std::printf("asset-resolve ok\n");
}

static void testLightingBookkeeping() {
    // Headless (no GL): exercises the light list and the flicker contract.
    // Flicker is applied at draw time, so `update()` must never mutate the
    // authored intensity (the old code compounded it every frame).
    LightingSystem lights;
    Light2D torch;
    torch.pos = {10, 20};
    torch.radius = 150.0f;
    torch.intensity = 1.5f;
    torch.flickerAmount = 0.2f;
    torch.flickerSpeed = 5.0f;
    torch.flickerPhase = 0.3f;
    int idx = lights.addLight(torch);
    CHECK(lights.lightCount() == 1);
    CHECK(lights.light(idx) != nullptr);
    for (int i = 0; i < 600; ++i) lights.update(1.0f / 60.0f);
    CHECK_CLOSE(lights.light(idx)->intensity, 1.5f, 1e-5f);
    CHECK_CLOSE(lights.light(idx)->pos.x, 10.0f, 1e-5f);
    CHECK(lights.light(-1) == nullptr);
    CHECK(lights.light(1) == nullptr);
    lights.clearLights();
    CHECK(lights.lightCount() == 0);
    std::printf("lighting ok\n");
}

static void testSkeleton() {
    // Parse a two-bone skeleton with one rotation track, then sample it.
    const char* json = R"JSON({
        "bones": [
            {"name": "root", "x": 0, "y": 0, "length": 10},
            {"name": "arm", "parent": "root", "x": 20, "y": 0, "rotation": 0,
             "length": 20}
        ],
        "slots": [
            {"name": "body", "bone": "root", "texture": "body", "width": 32,
             "height": 32, "order": 0},
            {"name": "hand", "bone": "arm", "texture": "hand", "width": 8,
             "height": 8, "offsetX": 20, "order": 1}
        ],
        "animations": [
            {"name": "wave", "duration": 1.0, "loop": true, "tracks": [
                {"bone": "arm", "type": "rotation", "keys": [
                    {"time": 0.0, "x": 0.0},
                    {"time": 0.5, "x": 1.0},
                    {"time": 1.0, "x": 0.0}
                ]}
            ]}
        ]
    })JSON";
    Skeleton sk;
    CHECK(sk.parseJson(json, "test"));
    CHECK(sk.valid());
    CHECK(sk.boneCount() == 2);
    CHECK(sk.slotCount() == 2);
    CHECK(sk.findBone("arm") == 1);
    CHECK(sk.findSlot("hand") == 1);
    CHECK(sk.animationCount() == 1);
    CHECK(sk.play("wave"));
    CHECK(sk.playing());
    CHECK(sk.currentAnimation() == "wave");

    sk.update(0.5f);
    CHECK_CLOSE(sk.time(), 0.5f, 1e-5f);
    CHECK_CLOSE(sk.bone(1)->worldRotation, 1.0f, 1e-4f);
    CHECK_CLOSE(sk.bone(1)->worldPos.x, 20.0f, 1e-4f);

    // Loops back to the start of the clip.
    sk.update(0.6f);
    CHECK_CLOSE(sk.time(), 0.1f, 1e-4f);
    CHECK_CLOSE(sk.bone(1)->worldRotation, 0.2f, 1e-4f);

    // Unknown clip names are ignored, not fatal.
    CHECK(!sk.play("nope"));
    CHECK(sk.currentAnimation() == "wave");
    // stop() drops back to the rest pose.
    sk.stop();
    sk.applyPose();
    CHECK_CLOSE(sk.bone(1)->worldRotation, 0.0f, 1e-5f);

    // Malformed JSON fails cleanly.
    Skeleton bad;
    CHECK(!bad.parseJson("{ not json", "bad"));
    CHECK(!bad.valid());
    std::printf("skeleton ok\n");
}

static void testSerialization() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "forge-test-serialization";
    fs::create_directories(dir);
    std::string scenePath = (dir / "scene.json").string();
    std::string prefabPath = (dir / "prefab.json").string();

    Scene scene;
    Entity hero = scene.create("hero");
    scene.registry().get<Transform>(hero).pos = {12, 34};
    scene.registry().get<Transform>(hero).rotation = 0.5f;
    Sprite sp;
    sp.src = {0, 0, 16, 16};
    sp.color = {1, 0, 0, 1};
    sp.layer = 3;
    scene.registry().emplace<Sprite>(hero, sp);
    scene.registry().emplace<Velocity>(hero, Velocity{{1, 2}, {0, 0}, 0.1f, 100});
    Collider col;
    col.halfExtents = {8, 8};
    col.sensor = true;
    scene.registry().emplace<Collider>(hero, col);

    Entity prop = scene.create("prop");
    scene.registry().get<Transform>(prop).pos = {-5, 0};

    CHECK(saveScene(scene, scenePath));
    Scene loaded;
    SceneSaveStats stats;
    CHECK(loadScene(loaded, scenePath, nullptr, &stats));
    CHECK(stats.entities == 2);
    CHECK(stats.sprites == 1);
    CHECK(stats.colliders == 1);
    Entity h2 = loaded.findByTag("hero");
    CHECK(h2 != kNullEntity);
    CHECK_CLOSE(loaded.registry().get<Transform>(h2).pos.x, 12.0f, 1e-4f);
    CHECK_CLOSE(loaded.registry().get<Transform>(h2).rotation, 0.5f, 1e-4f);
    CHECK(loaded.registry().has<Sprite>(h2));
    CHECK(loaded.registry().get<Sprite>(h2).layer == 3);
    CHECK(loaded.registry().get<Collider>(h2).sensor);
    CHECK_CLOSE(loaded.registry().get<Velocity>(h2).maxSpeed, 100.0f, 1e-4f);

    // Prefabs round-trip and instantiate at an overridden position.
    Prefab prefab = makePrefab(scene, hero, "hero_prefab");
    CHECK(prefab.valid());
    CHECK(savePrefab(prefab, prefabPath));
    Prefab loadedPrefab;
    CHECK(loadPrefab(loadedPrefab, prefabPath));
    CHECK(loadedPrefab.name == "hero_prefab");
    Scene target;
    Entity inst = instantiate(target, loadedPrefab, {100, 200}, nullptr);
    CHECK(inst != kNullEntity);
    CHECK_CLOSE(target.registry().get<Transform>(inst).pos.x, 100.0f, 1e-4f);
    CHECK_CLOSE(target.registry().get<Transform>(inst).pos.y, 200.0f, 1e-4f);
    CHECK(target.registry().has<Sprite>(inst));

    // Bad files are rejected without throwing.
    Scene bad;
    CHECK(!loadScene(bad, (dir / "missing.json").string()));
    std::FILE* f = std::fopen((dir / "bad.json").string().c_str(), "wb");
    if (f) { std::fputs("{\"format\":\"nope\"}", f); std::fclose(f); }
    CHECK(!loadScene(bad, (dir / "bad.json").string()));

    fs::remove_all(dir);
    std::printf("serialization ok\n");
}

static void testFontUtf8() {
    // Decode: ASCII, 2/3/4-byte sequences, invalid byte -> U+FFFD.
    auto cps = Font::decodeUtf8("A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80");
    CHECK(cps.size() == 4);
    CHECK(cps[0] == 'A');
    CHECK(cps[1] == 0xE9);
    CHECK(cps[2] == 0x20AC);
    CHECK(cps[3] == 0x1F600);
    auto bad = Font::decodeUtf8("a\xFF" "b");
    CHECK(bad.size() == 3 && bad[1] == 0xFFFD);
    // Round-trip.
    std::string enc;
    for (uint32_t cp : cps) Font::encodeUtf8(cp, enc);
    CHECK(enc == "A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80");
    // Empty input.
    CHECK(Font::decodeUtf8("").empty());
    std::printf("font-utf8 ok\n");
}

static void testInputDepth() {
    Input in;
    // Touch lifecycle.
    in.touchDown(1, {10, 20}, 0.5f);
    in.touchDown(2, {30, 40});
    CHECK(in.touchActive());
    CHECK(in.touches().size() == 2);
    CHECK(in.touchPressed());
    CHECK(in.touch(2) != nullptr);
    CHECK_CLOSE(in.touch(2)->pos.x, 30.0f, 1e-5f);
    in.touchMove(1, {15, 25}, 0.9f);
    CHECK_CLOSE(in.touchDrag(1).x, 5.0f, 1e-5f);
    CHECK_CLOSE(in.touch(1)->pressure, 0.9f, 1e-5f);
    in.touchUp(1);
    CHECK(in.touches().size() == 1);
    CHECK(in.touchReleased());
    in.touchCancel(2);
    CHECK(!in.touchActive());
    CHECK(in.touch(1) == nullptr);
    in.endFrame();
    CHECK(!in.touchPressed());

    // IME composition state.
    in.setComposition("\xE3\x81\x82", 3);
    CHECK(in.composing());
    CHECK(in.compositionCursor() == 3);
    in.endFrame();
    CHECK(!in.composing());

    // Action remap round-trip through the text format and a file.
    in.bindKey("jump", Key::Space);
    in.bindKey("jump", Key::Z);
    in.bindMouse("jump", MouseButton::Left);
    in.bindPad("jump", GamepadButton::A);
    in.bindKey("left", Key::A);
    std::string text = in.serializeBinds();
    CHECK(text.find("jump=") != std::string::npos);
    CHECK(text.find("mouse:left") != std::string::npos);
    CHECK(text.find("pad:a") != std::string::npos);

    Input remapped;
    std::string err;
    CHECK(remapped.deserializeBinds(text, &err));
    CHECK(err.empty());
    remapped.setKey(Key::Space, true);
    CHECK(remapped.actionDown("jump"));
    CHECK(remapped.actionPressed("jump"));
    remapped.setKey(Key::Space, false);
    CHECK(remapped.actionReleased("jump"));

    // Unknown tokens are rejected with a line number.
    Input bad;
    CHECK(!bad.deserializeBinds("jump=notakey\n", &err));
    CHECK(err.find("line 1") != std::string::npos);

    namespace fs = std::filesystem;
    fs::path path = fs::temp_directory_path() / "forge-test-binds.txt";
    CHECK(in.saveBinds(path.string()));
    Input fromFile;
    CHECK(fromFile.loadBinds(path.string(), &err));
    CHECK(fromFile.actionDown("nothing") == false);
    fs::remove(path);

    // Deadzone/curve setters clamp.
    in.setStickDeadzone(2.0f);
    CHECK_CLOSE(in.stickDeadzone(), 1.0f, 1e-5f);
    in.setStickCurve(-1.0f);
    CHECK_CLOSE(in.stickCurve(), 0.0f, 1e-5f);
    std::printf("input-depth ok\n");
}

int main() {
    std::printf("forge %s (tests)\n", FORGE_VERSION);
    testMath();
    testMathExtra();
    testEcs();
    testEcsLimits();
    testPhysics();
    testPhysicsExtra();
    testPhysicsSolver();
    testPhysicsFeatures();
    testScenePhysics();
    testCamera();
    testInput();
    testInputSurface();
    testInputStepWindow();
    testTilemap();
    testTilemapJson();
    testAnim();
    testAudioHeadless();
    testScene();
    testJson();
    testScript();
    testAtlas();
    testAssetResolve();
    testSkeleton();
    testLightingBookkeeping();
    testSerialization();
    testFontUtf8();
    testInputDepth();
    if (failures == 0) std::printf("ALL TESTS PASSED\n");
    else std::printf("%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
