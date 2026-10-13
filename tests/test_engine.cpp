// Headless unit tests: math, ECS, physics, tilemap-data, animation/tween.
// No GL context needed. Returns nonzero on first failure.
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <vector>
#include <fstream>
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
    const char* tmp = "/tmp/opencode/forge_test_map.json";
    { std::ofstream f(tmp); f << json; }
    Tilemap m;
    CHECK(m.loadTiledJson(tmp));
    CHECK(m.width() == 3 && m.height() == 2);
    CHECK(m.tilesetPath() == "ts.png");
    // Tiled row 0 (top): [0,6,0] -> Forge y=1; row 1: [7,0,flip|9] -> y=0.
    // firstgid 5: gid 6 -> local 2, gid 7 -> local 3, gid 9 -> local 5.
    CHECK(m.tile(0, 1, 1) == 2);
    CHECK(m.tile(0, 0, 0) == 3);
    CHECK(m.tile(0, 2, 0) == 5); // 0x80000009 masked to gid 9
    CHECK(m.tile(0, 0, 1) == 0);
    // Missing keys must fail cleanly, not segfault (old code deref'd null).
    const char* bad = "/tmp/opencode/forge_test_bad.json";
    { std::ofstream f(bad); f << "{\"layers\": []}"; }
    Tilemap m2;
    CHECK(!m2.loadTiledJson(bad));
    CHECK(!m2.loadTiledJson("/tmp/opencode/does_not_exist.json"));
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
    scene.registry().emplace<Script>(e, Script{
        [](Entity self, Registry& reg, float) { reg.destroy(self); }});
    scene.update(0.016f); // would UAF inside view before fix
    CHECK(!scene.registry().alive(e));
    scene.update(0.016f); // no scripts left, still safe
    std::printf("scene ok\n");
}

int main() {
    testMath();
    testMathExtra();
    testEcs();
    testPhysics();
    testPhysicsExtra();
    testCamera();
    testInput();
    testTilemap();
    testTilemapJson();
    testAnim();
    testAudioHeadless();
    testScene();
    if (failures == 0) std::printf("ALL TESTS PASSED\n");
    else std::printf("%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
