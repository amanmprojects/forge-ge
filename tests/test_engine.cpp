// Headless unit tests: math, ECS, physics, tilemap-data, animation/tween.
// No GL context needed. Returns nonzero on first failure.
#include <cstdio>
#include <cmath>
#include "forge/math/math.hpp"
#include "forge/scene/ecs.hpp"
#include "forge/scene/components.hpp"
#include "forge/physics/physics.hpp"
#include "forge/graphics/tilemap.hpp"
#include "forge/graphics/animation.hpp"

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
    std::printf("ecs ok\n");
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

    // CSV loader
    Tilemap m2;
    CHECK(m2.loadCsv("1,0,0,0, 0,2,0,0, 0,0,0,0, 0,0,0,3", 4, 4, 8, 8));
    CHECK(m2.tile(0, 0, 0) == 1);
    CHECK(m2.tile(0, 1, 1) == 2);
    CHECK(m2.tile(0, 3, 3) == 3);
    std::printf("tilemap ok\n");
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
    std::printf("anim ok\n");
}

int main() {
    testMath();
    testEcs();
    testPhysics();
    testTilemap();
    testAnim();
    if (failures == 0) std::printf("ALL TESTS PASSED\n");
    else std::printf("%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
