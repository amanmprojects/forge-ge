// 02_platformer — physics + procedural tilemap + particles + camera follow/shake.
//
//   move  A/D · arrows · left stick      jump  SPACE · W · up · gamepad A
//   reset R                              quit  ESC
//
//   ./02_platformer --demo                          # autopilot climbs the platforms
//   ./02_platformer --record clip.mp4 --frames 900  # autopilot video (ffmpeg)
// CI=1 ./02_platformer                              # 600-frame smoke run
#include <forge/forge.hpp>

#include "clip.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int TW = 32;              // tile size
constexpr int MW = 40, MH = 12;     // map size in tiles
constexpr float SPEED = 320.0f;
constexpr float JUMP_VEL = 780.0f;
const forge::Vec2 kSpawn{100, 200};

struct Platformer {
    forge::Engine* engine = nullptr;
    forge::Texture2D tileset;
    forge::Tilemap map;
    forge::PhysicsWorld world;
    uint32_t playerId = 0;
    int landings = 0;
    forge::ParticleSystem particles;
    forge::Camera2D cam;
    bool wasGround = false;
    bool autopilot = false;
    float demoDir = 1.0f; // autopilot heading: +1 right (climbing), -1 back

    void init(forge::Engine& eng) {
        engine = &eng;
        buildTileset();
        buildMap();
        buildWorld();
        initParticles();
        cam.zoom = 2.0f;
    }

    // --- procedural tileset: 4 tiles of 32px in a 128x32 strip ---
    void buildTileset() {
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
        // NOTE: tilemap UVs assume flipY=true load; build rows flipped manually:
        // (we upload bottom-up so tile row math stays top-down)
        std::vector<uint8_t> flipped(px.size());
        for (int y = 0; y < 32; ++y)
            std::memcpy(&flipped[(size_t)y * 128 * 4], &px[(size_t)(31 - y) * 128 * 4], 128 * 4);
        tileset.loadMemory(flipped.data(), 128, 32, 4, forge::TextureFilter::Nearest);
    }

    // --- map: 40x12 CSV, row 0 solid + platforms ---
    void buildMap() {
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
        map.loadCsv(csv, MW, MH, TW, TW);
        map.setTileset(&tileset, 4, 4);
        for (int i = 1; i <= 4; ++i) map.setSolid(i, true);
    }

    // --- physics bodies from solid tiles ---
    void buildWorld() {
        world.setGravity({0, -2200});
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
        forge::Body player;
        player.type = forge::BodyType::Dynamic;
        player.pos = kSpawn;
        player.size = {22, 28};
        player.friction = 0.25f;
        playerId = world.addBody(player);

        world.onContact([this](const forge::Contact& c) {
            if ((c.a == playerId || c.b == playerId) && std::fabs(c.normal.y) > 0.7f)
                ++landings;
        });
    }

    // --- dust particles ---
    void initParticles() {
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
        particles.configure(dust);
    }

    // Autopilot: run right and hop up the three platforms (each jump point
    // leaves room to clear the platform's lip), turn at the top, walk back
    // down to the spawn and repeat. Heights are the player's center when
    // standing on the ground / A / B / C (tile tops + half the body).
    void demoInput(const forge::Body& p, float& move, bool& jump) {
        const float x = p.pos.x, y = p.pos.y;
        if (demoDir > 0) {
            if (p.onGround) {
                const bool ground = y < 90, onA = y > 120 && y < 170, onB = y > 180 && y < 235,
                           onC = y > 250;
                if (ground && x > 110 && x < 135) jump = true;
                if (onA && x >= 372) jump = true;
                if (onB && x >= 815) jump = true;
                if ((onC && x >= 1120) || (ground && x > 1100)) demoDir = -1;
            }
        } else if (x <= 80) {
            demoDir = 1;
        }
        move = demoDir;
    }

    void update(float dt) {
        auto& in = engine->input();
        if (in.isDown(forge::Key::Escape)) engine->quit();
        forge::Body* p = world.get(playerId);
        float move = 0;
        bool jump = false;
        if (autopilot) {
            demoInput(*p, move, jump);
        } else {
            if (in.isDown(forge::Key::A) || in.isDown(forge::Key::Left)) move -= 1;
            if (in.isDown(forge::Key::D) || in.isDown(forge::Key::Right)) move += 1;
            move += in.moveAxis(0).x;
            jump = in.pressed(forge::Key::Space) || in.pressed(forge::Key::W) ||
                   in.pressed(forge::Key::Up) || in.padPressed(0, forge::GamepadButton::A);
        }
        p->vel.x = move * SPEED;
        if (jump && p->onGround) {
            p->vel.y = JUMP_VEL;
            particles.setPosition(p->pos + forge::Vec2{0, -14});
            particles.emit(12);
            cam.addTrauma(0.12f);
        }
        if (in.pressed(forge::Key::R)) { // reset
            p->pos = kSpawn;
            p->vel = {0, 0};
        }
        world.step(dt);
        p = world.get(playerId);
        particles.setPosition(p->pos);
        particles.update(dt);
        if (p->onGround && !wasGround) { // landed
            particles.setPosition(p->pos + forge::Vec2{0, -14});
            particles.emit(8);
        }
        wasGround = p->onGround;
        cam.follow(p->pos + forge::Vec2{0, 40}, 5.0f, dt);
        cam.update(dt);
    }

    // fb == nullptr renders to the window; a target renders offscreen (the
    // --record path, so clips are independent of window-manager sizing).
    void renderTo(forge::Framebuffer* fb) {
        forge::IVec2 vp = fb ? forge::IVec2{fb->width(), fb->height()} : engine->viewport();
        if (fb) fb->bind();
        forge::gl::glViewport(0, 0, vp.x, vp.y);
        forge::gl::glClearColor(0.07f, 0.09f, 0.14f, 1.0f);
        forge::gl::glClear(GL_COLOR_BUFFER_BIT);
        auto& batch = engine->batch();
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
        if (fb) forge::Framebuffer::unbind(engine->viewport().x, engine->viewport().y);
    }
};

struct Args {
    std::string record;
    int frames = 0;
    int width = 1280, height = 720;
    bool demo = false;
};

Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (arg == "--record") { if (const char* v = next()) a.record = v; }
        else if (arg == "--frames") { if (const char* v = next()) a.frames = std::atoi(v); }
        else if (arg == "--size") {
            if (const char* v = next()) std::sscanf(v, "%dx%d", &a.width, &a.height);
        }
        else if (arg == "--demo") a.demo = true;
    }
    if (a.width < 64) a.width = 64;
    if (a.height < 64) a.height = 64;
    return a;
}

} // namespace

int main(int argc, char** argv) {
    const Args args = parseArgs(argc, argv);

    forge::EngineConfig cfg;
    cfg.window.title = "Forge 02_platformer";
    cfg.window.vsync = args.record.empty();
#if FORGE_PLATFORM_WEB
    // The drawing buffer takes the canvas's CSS size once and then stays put.
    cfg.window.resizable = false;
    // Emscripten takes the frame loop over by unwinding main()'s stack, and
    // with wasm exceptions on that unwind runs the destructors of main()'s
    // locals (a unique_ptr owner would free the game under the loop). Statics
    // outlive main and are never destroyed: the web runtime never exits.
    static forge::Engine engine(cfg);
    static Platformer game;
#else
    forge::Engine engine(cfg);
    Platformer game;
#endif
    if (!engine.valid()) return 1;
    game.init(engine);

#if FORGE_PLATFORM_WEB
    // Runs until the tab closes. Nothing to capture: the loop touches only the
    // statics, and main()'s locals are gone by frame one.
    engine.runFixed([](float dt) { game.update(dt); }, [] { game.renderTo(nullptr); });
#else
    if (!args.record.empty()) {
        game.autopilot = true;
        const bool ok = clip::recordClip(
            args.record, args.width, args.height, args.frames > 0 ? args.frames : 900,
            [&] { game.update(1.0f / 60.0f); },
            [&](forge::Framebuffer* fb) { game.renderTo(fb); }, engine);
        std::printf("landings detected: %d\n", game.landings);
        return ok ? 0 : 1;
    }

    game.autopilot = args.demo;
    int frames = 0;
    engine.runFixed(
        [&](float dt) { game.update(dt); },
        [&]() {
            game.renderTo(nullptr);
            if (++frames >= 600) { // auto-exit for CI; remove for play
                if (std::getenv("CI")) engine.quit();
                frames = 0;
            }
        });
    std::printf("landings detected: %d\n", game.landings);
#endif
    return 0;
}
