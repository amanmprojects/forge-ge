// 02_platformer — physics + procedural tilemap + particles + camera follow/shake.
#include <forge/forge.hpp>
#include <cstdio>
#include <vector>
#include <string>
#include <cstring>
#include <cmath>

int main() {
    forge::EngineConfig cfg;
    cfg.window.title = "Forge 02_platformer";
    forge::Engine engine(cfg);
    if (!engine.valid()) return 1;

    // --- procedural tileset: 4 tiles of 32px in a 128x32 strip ---
    const int TW = 32;
    std::vector<uint8_t> px(128 * 32 * 4);
    auto paint = [&](int tile, forge::Color c, bool top) {
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x) {
                size_t i = ((size_t)y * 128 + tile * 32 + x) * 4;
                forge::Color cc = c;
                if (top && y < 6) cc = forge::Color::fromHex(0x90BE6DFF); // grass lip
                px[i + 0] = (uint8_t)(cc.r * 255);
                px[i + 1] = (uint8_t)(cc.g * 255);
                px[i + 2] = (uint8_t)(cc.b * 255);
                px[i + 3] = 255;
            }
    };
    paint(0, forge::Color::fromHex(0x43AA8BFF), true);
    paint(1, forge::Color::fromHex(0x577590FF), false);
    paint(2, forge::Color::fromHex(0xF94144FF), false);
    paint(3, forge::Color::fromHex(0xF9C74FFF), false);
    forge::Texture2D tileset;
    // NOTE: tilemap UVs assume flipY=true load; build rows flipped manually:
    // (we upload bottom-up so tile row math stays top-down)
    std::vector<uint8_t> flipped(px.size());
    for (int y = 0; y < 32; ++y)
        std::memcpy(&flipped[(size_t)y * 128 * 4], &px[(size_t)(31 - y) * 128 * 4], 128 * 4);
    tileset.loadMemory(flipped.data(), 128, 32, 4, forge::TextureFilter::Nearest);

    // --- map: 40x12 CSV, row 11 solid + platforms ---
    const int MW = 40, MH = 12;
    std::string csv;
    for (int y = 0; y < MH; ++y) {
        for (int x = 0; x < MW; ++x) {
            int id = 0;
            if (y == 0) id = 1;                       // ground (bottom row in our Y-up csv)
            if (y == 3 && x > 5 && x < 12) id = 2;    // platform
            if (y == 5 && x > 16 && x < 26) id = 3;
            if (y == 7 && x > 28 && x < 36) id = 4;
            csv += std::to_string(id);
            if (!(y == MH - 1 && x == MW - 1)) csv += ",";
        }
    }
    forge::Tilemap map;
    map.loadCsv(csv, MW, MH, TW, TW);
    map.setTileset(&tileset, 4, 4);
    for (int i = 1; i <= 4; ++i) map.setSolid(i, true);

    // --- physics bodies from solid tiles ---
    forge::PhysicsWorld world;
    world.setGravity({0, -2200});
    {
        // one static body per solid tile run (simple: per tile)
        for (int y = 0; y < MH; ++y)
            for (int x = 0; x < MW; ++x) {
                int id = map.tile(0, x, y);
                if (id != 0) {
                    forge::Body b;
                    b.type = forge::BodyType::Static;
                    b.pos = {(float)x * TW + TW / 2.0f, (float)y * TW + TW / 2.0f};
                    b.size = {(float)TW, (float)TW};
                    world.addBody(b);
                }
            }
    }
    forge::Body player;
    player.type = forge::BodyType::Dynamic;
    player.pos = {100, 200};
    player.size = {22, 28};
    player.friction = 0.25f;
    uint32_t playerId = world.addBody(player);

    int landings = 0;
    world.onContact([&](const forge::Contact& c) {
        if ((c.a == playerId || c.b == playerId) && std::fabs(c.normal.y) > 0.7f)
            ++landings;
    });

    // --- dust particles ---
    forge::EmitterConfig dust;
    dust.maxParticles = 512;
    dust.spawnRate = 0; // manual bursts
    dust.looping = false;
    dust.life = 0.5f; dust.lifeVar = 0.2f;
    dust.size = 6; dust.sizeVar = 3; dust.endSize = 1;
    dust.start = forge::Color::fromHex(0xF1FAEEAA);
    dust.end = forge::Color::clear();
    dust.vel = {0, 60}; dust.velVar = {90, 40};
    dust.damping = 2.0f;
    forge::ParticleSystem particles(dust);

    forge::Camera2D cam;
    cam.zoom = 2.0f;

    bool wasGround = false;
    engine.runFixed(
        [&](float dt) { // update
            auto& in = engine.input();
            if (in.isDown(forge::Key::Escape)) engine.quit();
            forge::Body* p = world.get(playerId);
            const float SPEED = 320.0f;
            float move = 0;
            if (in.isDown(forge::Key::A) || in.isDown(forge::Key::Left)) move -= 1;
            if (in.isDown(forge::Key::D) || in.isDown(forge::Key::Right)) move += 1;
            move += in.moveAxis(0).x;
            p->vel.x = move * SPEED;
            if ((in.pressed(forge::Key::Space) || in.pressed(forge::Key::W) ||
                 in.pressed(forge::Key::Up) || in.padPressed(0, forge::GamepadButton::A)) &&
                p->onGround) {
                p->vel.y = 780.0f;
                particles.setPosition(p->pos + forge::Vec2{0, -14});
                particles.emit(12);
                cam.addTrauma(0.12f);
            }
            if (in.pressed(forge::Key::R)) { // reset
                p->pos = {100, 200};
                p->vel = {0, 0};
            }
            world.step(dt);
            particles.setPosition(world.get(playerId)->pos);
            particles.update(dt);
            if (p->onGround && !wasGround) { // landed
                particles.setPosition(p->pos + forge::Vec2{0, -14});
                particles.emit(8);
            }
            wasGround = p->onGround;
            cam.follow(p->pos + forge::Vec2{0, 40}, 5.0f, dt);
            cam.update(dt);
        },
        [&]() { // render
            forge::IVec2 vp = engine.viewport();
            forge::gl::glViewport(0, 0, vp.x, vp.y);
            forge::gl::glClearColor(0.07f, 0.09f, 0.14f, 1.0f);
            forge::gl::glClear(GL_COLOR_BUFFER_BIT);
            auto& batch = engine.batch();
            batch.begin(cam.viewProj(vp.x, vp.y));

            // cull rect around camera
            forge::Vec2 half{(float)vp.x / cam.zoom / 2 + 64, (float)vp.y / cam.zoom / 2 + 64};
            map.draw(batch, {0, 0}, {cam.position.x - half.x, cam.position.y - half.y,
                                     half.x * 2, half.y * 2});
            particles.draw(batch);

            const forge::Body* p = world.get(playerId);
            forge::SpriteDraw d;
            d.pos = p->pos;
            d.size = p->size;
            d.color = forge::Color::fromHex(0xE9C46AFF);
            batch.draw(d);
            batch.drawRectOutline({p->pos.x - 11, p->pos.y - 14, 22, 28},
                                  forge::Color::white(), 1.5f);
            batch.end();
        });
    std::printf("landings detected: %d\n", landings);
    return 0;
}
