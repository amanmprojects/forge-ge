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

int main() {
    std::printf("forge %s (tests)\n", FORGE_VERSION);
    testMath();
    testMathExtra();
    testEcs();
    testEcsLimits();
    testPhysics();
    testPhysicsExtra();
    testPhysicsSolver();
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
    if (failures == 0) std::printf("ALL TESTS PASSED\n");
    else std::printf("%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
