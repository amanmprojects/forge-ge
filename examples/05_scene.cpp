// 05_scene — ECS scene + tweens + script components + Collider physics.
#include <forge/forge.hpp>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main() {
    forge::EngineConfig cfg;
    cfg.window.title = "Forge 05_scene";
    forge::Engine engine(cfg);
    if (!engine.valid()) return 1;

    forge::Texture2D orb;
    orb.createSolid(forge::Color::white());

    forge::Scene scene;
    forge::Tweens tweens;

    // spawn 60 orbiting entities with scripts
    for (int i = 0; i < 60; ++i) {
        forge::Entity e = scene.create(i == 0 ? "hero" : "");
        auto& t = scene.registry().get<forge::Transform>(e);
        float a = i * forge::TAU / 60;
        t.pos = {std::cos(a) * 250, std::sin(a) * 250};
        t.scale = {1, 1};
        auto& s = scene.registry().emplace<forge::Sprite>(e);
        s.texture = &orb;
        s.size = {18, 18};
        s.color = forge::Color{0.2f + 0.8f * (i % 3 == 0), 0.4f, 0.9f, 1.0f};
        s.layer = i % 4;
        auto& v = scene.registry().emplace<forge::Velocity>(e);
        v.v = {-t.pos.y, t.pos.x};
        v.v = v.v.normalized() * 60.0f;
        float speed = 40.0f + (i % 5) * 20.0f;
        forge::Script orbit;
        orbit.onUpdate = [speed](forge::Entity ent, forge::Registry& reg, float dt) {
            auto& tr = reg.get<forge::Transform>(ent);
            float ang = std::atan2(tr.pos.y, tr.pos.x) + speed * 0.002f * dt;
            float r = tr.pos.length();
            tr.pos = {std::cos(ang) * r, std::sin(ang) * r};
            tr.rotation += dt;
        };
        scene.registry().emplace<forge::Script>(e, std::move(orbit));
    }

    // Collider demo: Scene syncs Transform+Collider entities into its
    // PhysicsWorld (Velocity present -> dynamic; here a crate lands on a
    // static floor).
    forge::Entity floor = scene.create("floor");
    scene.registry().get<forge::Transform>(floor).pos = {0, -260};
    scene.registry().emplace<forge::Collider>(floor, forge::Collider{{380, 20}});
    auto& fs = scene.registry().emplace<forge::Sprite>(floor);
    fs.texture = &orb; fs.size = {760, 40}; fs.color = {0.35f, 0.35f, 0.45f, 1.0f};

    for (int i = 0; i < 5; ++i) {
        forge::Entity c = scene.create("crate");
        scene.registry().get<forge::Transform>(c).pos = {-160.0f + i * 80.0f, 200.0f + i * 40.0f};
        forge::Collider col;
        col.halfExtents = {12, 12};
        col.friction = 0.4f;
        scene.registry().emplace<forge::Collider>(c, col);
        scene.registry().emplace<forge::Velocity>(c);
        auto& s = scene.registry().emplace<forge::Sprite>(c);
        s.texture = &orb; s.size = {24, 24}; s.color = {0.9f, 0.6f, 0.2f, 1.0f};
    }

    // scale-punch tween on demand
    forge::Camera2D cam;
    int frames = 0;
    engine.runVariable([&](float dt) {
        auto& in = engine.input();
        if (in.isDown(forge::Key::Escape)) engine.quit();
        if (in.pressed(forge::Key::Space)) {
            forge::Tween tw;
            tw.from = 1; tw.to = 2; tw.duration = 0.18f;
            tw.mode = forge::TweenMode::PingPong;
            tw.ease = forge::easing::outBack;
            tw.onUpdate = [&](float v) { cam.zoom = v; };
            tweens.add(tw);
            cam.addTrauma(0.3f);
        }
        scene.update(dt);
        tweens.update(dt);
        cam.update(dt);

        forge::IVec2 vp = engine.viewport();
        forge::gl::glViewport(0, 0, vp.x, vp.y);
        forge::gl::glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
        forge::gl::glClear(GL_COLOR_BUFFER_BIT);
        engine.batch().begin(cam.viewProj(vp.x, vp.y));
        scene.draw(engine.batch());
        engine.batch().end();
        if (++frames >= 600) { // PPP: auto-exit for CI; remove for play
            // keep running interactively unless CI env set
            if (std::getenv("CI")) engine.quit();
            frames = 0;
        }
    });

    std::printf("entities: %zu\n", scene.registry().entityCount());
    return 0;
}
