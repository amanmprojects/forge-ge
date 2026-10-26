// 10_lantern — the integration example: every Phase 0–4 subsystem in one
// small, complete game.
//
//   move     WASD · arrows · left stick · touch drag
//   collect  walk over the lanterns
//   save/load F5 / F9 (JSON scene file)
//   reload script  F2 (assets/lantern.fgs, hot-swapped while running)
//   overlay  F1      debug wireframe F3      quit Q / ESC
//
// What it exercises:
// - Tilemap: CSV layers, multi-tile autotile, a slope tile, view culling.
// - LightingSystem: dark ambient + player lantern + per-gem lights + shadows.
// - PostProcess: vignette over the composed frame.
// - Scene + serialization: gems/player are entities; F5/F9 round-trip them.
// - forge::script: the score rule and the objective line live in a .fgs file.
// - DebugOverlay: F1 panel with FPS graph, counts and the script's state.
// - Input depth: action remaps (persisted to disk), touch drag, deadzone.
// - Framebuffer/--shot + CI=1: deterministic capture and a smoke run.
//   --record clip.mp4 renders the autopilot to a video (ffmpeg).
#include <forge/forge.hpp>

#include "clip.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wcast-qual"
#endif
#include "stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace {

using forge::Color;
using forge::Rect;
using forge::Vec2;

constexpr int TILE = 32;
constexpr int MAP_W = 30, MAP_H = 18;
constexpr float VIEW_W = MAP_W * TILE;   // 960
constexpr float VIEW_H = MAP_H * TILE;   // 576
constexpr float PLAYER_R = 11.0f;
constexpr float PLAYER_SPEED = 190.0f;

// Tile ids (within the single tileset): 1 floor, 2 wall, 3 ramp, 4 trim.
enum : int { T_FLOOR = 1, T_WALL = 2, T_RAMP = 3, T_TRIM = 4 };

const Color kBg = Color::fromHex(0x0A0C12FF);
const Color kAmbient = Color::fromHex(0x232C40FF);
const Color kLantern = Color::fromHex(0xFFD9A0FF);
const Color kGem = Color::fromHex(0x7DE2FFFF);
const Color kHud = Color::fromHex(0xE6EDF7FF);
const Color kHudDim = Color::fromHex(0x7C8AA5FF);
const Color kAccent = Color::fromHex(0xFFB74DFF);

// --------------------------------------------------------------- tileset
// 4 tiles of 32px in a 128x32 strip, uploaded bottom-up (tilemap UVs assume
// flipY=true loads, so we flip the source rows once here).
bool buildTileset(forge::Texture2D& out) {
    const int N = 4, W = N * TILE, H = TILE;
    std::vector<uint8_t> px((size_t)W * H * 4, 0);
    auto set = [&](int x, int y, Color c) {
        if (x < 0 || y < 0 || x >= W || y >= H) return;
        size_t i = ((size_t)y * W + x) * 4;
        px[i + 0] = (uint8_t)(forge::clamp01(c.r) * 255);
        px[i + 1] = (uint8_t)(forge::clamp01(c.g) * 255);
        px[i + 2] = (uint8_t)(forge::clamp01(c.b) * 255);
        px[i + 3] = (uint8_t)(forge::clamp01(c.a) * 255);
    };
    auto tile = [&](int id, Color base) {
        int ox = id * TILE;
        for (int y = 0; y < TILE; ++y)
            for (int x = 0; x < TILE; ++x) {
                // subtle checker + mortar so tiles read at 1x
                Color c = base;
                if (((x / 8) + (y / 8)) & 1) c = Color{base.r * 1.08f, base.g * 1.08f, base.b * 1.08f, 1};
                if (x == 0 || y == 0) c = Color{base.r * 0.72f, base.g * 0.72f, base.b * 0.72f, 1};
                set(ox + x, y, c);
            }
    };
    tile(0, Color::fromHex(0x7688A8FF)); // floor
    tile(1, Color::fromHex(0x3E4B66FF)); // wall
    tile(2, Color::fromHex(0x8FA6C8FF)); // ramp
    tile(3, Color::fromHex(0x9A7A52FF)); // trim
    // Ramp gets a bright walkable lip along its diagonal.
    for (int x = 0; x < TILE; ++x) {
        int y = TILE - 1 - x;
        set(2 * TILE + x, y, Color::fromHex(0xE8B96AFF));
        set(2 * TILE + x, y - 1, Color::fromHex(0x8A6A3AFF));
    }
    // Flip rows: the tilemap's UV math expects the flipY=true convention.
    std::vector<uint8_t> flipped(px.size());
    for (int y = 0; y < H; ++y)
        std::memcpy(&flipped[(size_t)y * W * 4], &px[(size_t)(H - 1 - y) * W * 4],
                    (size_t)W * 4);
    return out.loadMemory(flipped.data(), W, H, 4, forge::TextureFilter::Nearest);
}

// ------------------------------------------------------------------ script
const char* kDefaultScript = R"FGS(
// ForgeScript: the game's score + objective rules. Edit and press F2.
var collected = 0;
var bonus = 0;

fn reset() {
    collected = 0;
    bonus = 0;
    return 0;
}

// Called once per lantern. Fast pickups in a row earn a growing bonus.
fn on_collect(streak) {
    collected = collected + 1;
    if (streak >= 3) { bonus = bonus + 5; }
    return collected * 10 + bonus;
}

fn total() { return collected; }

fn objective(left) {
    if (left <= 0) { return "All lanterns lit. F5 saves, F9 loads."; }
    if (left == 1) { return "One lantern left..."; }
    return "Find the lanterns";
}

fn light_radius(base) {
    // The lantern grows a little with every pickup.
    return base + min(collected * 6, 60);
}
)FGS";

// ------------------------------------------------------------------ game
struct Game {
    forge::Engine* engine = nullptr;
    forge::Camera2D cam;
    forge::Tilemap map;
    forge::Texture2D tileset;
    forge::Scene scene;
    forge::LightingSystem lighting;
    forge::PostProcess post;
    forge::Framebuffer sceneFbo; // offscreen scene target when post is enabled
    forge::Texture2D sceneView;  // non-owning view of sceneFbo's color texture
    forge::DebugOverlay overlay;
    forge::script::ScriptVM vm;
    forge::Font font;
    forge::Font fontBig;

    bool haveFont = false, haveBig = false;
    bool overlayReady = false;
    bool lightingReady = false, postReady = false;
    bool wireframe = false;
    bool showHelp = true;

    forge::Entity player = forge::kNullEntity;
    std::vector<forge::Entity> gems;
    int score = 0;
    int streak = 0;
    float streakTimer = 0.0f;
    float time = 0.0f;
    float lastDt = 1.0f / 60.0f;
    float bob = 0.0f;
    std::string message = "Find the lanterns";
    float messageTimer = 0.0f;
    forge::IVec2 viewport{1280, 768};
    std::string scriptPath = "assets/lantern.fgs";
    bool scriptFromFile = false;

    // Deterministic state for --shot: the sim never reads the wall clock.
    bool healthy = true;
    // Scripted player for --shot / CI: walks to the nearest lantern so the
    // capture and the smoke run exercise pickups, lighting and scoring.
    bool autopilot = false;

    bool init(forge::Engine& eng) {
        engine = &eng;
        cam.zoom = 1.0f;
        cam.position = {VIEW_W * 0.5f, VIEW_H * 0.5f};

        // --- font (system TTF; text is optional, never fatal) ---
        haveBig = loadFont(fontBig, 40.0f);
        haveFont = loadFont(font, 18.0f) || haveBig;
        if (!haveBig) fontBig = forge::Font{};

        // --- tileset + map ---
        if (!buildTileset(tileset)) return false;
        buildMap();

        // --- lighting / post ---
        lightingReady = lighting.init(viewport.x, viewport.y);
        lighting.setAmbient(kAmbient);
        postReady = post.init(forge::PostProcess::Effect::Vignette);
        post.setVignette(0.55f, 0.72f);
        post.setScanlines(0.04f);

        // --- scripting ---
        vm.printTo([](const std::string& s) { FORGE_LOG_INFO("[fgs] %s", s.c_str()); });
        vm.bind("log", [](const std::vector<forge::script::Value>& a) {
            std::string s;
            for (auto& v : a) s += v.toString();
            FORGE_LOG_INFO("[fgs] %s", s.c_str());
            return forge::script::Value();
        });
        loadScript();

        // --- input remaps (persisted) ---
        auto& in = engine->input();
        in.setStickDeadzone(0.22f);
        in.bindKey("left", forge::Key::A);
        in.bindKey("left", forge::Key::Left);
        in.bindKey("right", forge::Key::D);
        in.bindKey("right", forge::Key::Right);
        in.bindKey("up", forge::Key::W);
        in.bindKey("up", forge::Key::Up);
        in.bindKey("down", forge::Key::S);
        in.bindKey("down", forge::Key::Down);
        in.loadBinds("lantern.binds");

        // --- overlay ---
        if (haveFont) overlayReady = overlay.init(&font);

        newRun();
        return true;
    }

    static bool loadFont(forge::Font& f, float px) {
        const char* system[] = {
            "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
            "/usr/share/fonts/Adwaita/AdwaitaMono-Regular.ttf",
            "/usr/share/fonts/liberation/LiberationMono-Regular.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
            "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
            "/System/Library/Fonts/Menlo.ttc",
            "C:/Windows/Fonts/consola.ttf",
        };
        for (const char* p : system)
            if (f.loadFile(p, px)) return true;
        return false;
    }

    void buildMap() {
        map.create(MAP_W, MAP_H, TILE, TILE);
        map.setTileset(&tileset, 4, 4);
        // Floor is the walkable background; only walls/ramps/trim collide.
        map.setSolid(T_FLOOR, false);
        map.setSolid(T_WALL, true);
        map.setSolid(T_RAMP, true);
        map.setSolid(T_TRIM, true);
        // A 45-degree ramp on the right side: low on the left, high on the
        // right. collideRect() resolves against its surface.
        map.setSlope(T_RAMP, 0.0f, 1.0f);

        for (int y = 0; y < MAP_H; ++y) {
            for (int x = 0; x < MAP_W; ++x) {
                bool border = x == 0 || y == 0 || x == MAP_W - 1 || y == MAP_H - 1;
                int id = border ? T_WALL : T_FLOOR;
                // interior pillars
                if (!border && (x % 7 == 3) && (y % 5 == 2)) id = T_WALL;
                // a raised platform with trim and a ramp up to it
                if (y == 11 && x >= 18 && x <= 24) id = T_WALL;
                if (y == 12 && x == 18) id = T_RAMP;
                if (y == 11 && x == 24) id = T_TRIM;
                map.setTile(0, x, y, id);
            }
        }
    }

    void loadScript() {
        bool ok = vm.loadFile(scriptPath);
        scriptFromFile = ok;
        if (!ok) {
            ok = vm.loadSource(kDefaultScript, "<builtin>");
            scriptFromFile = false;
        }
        if (!ok) {
            message = "Script error: " + vm.error().str();
            messageTimer = 4.0f;
        }
    }

    void newRun() {
        scene.clear();
        gems.clear();
        score = 0;
        streak = 0;
        streakTimer = 0;
        vm.call("reset");

        player = scene.create("player");
        scene.registry().get<forge::Transform>(player).pos = {3 * TILE, 3 * TILE};
        scene.registry().emplace<forge::ScriptRef>(player, forge::ScriptRef{"player"});

        // Lanterns on a deterministic lattice, away from walls.
        for (int y = 3; y < MAP_H - 2; y += 4) {
            for (int x = 4; x < MAP_W - 3; x += 5) {
                if (map.tile(0, x, y) != T_FLOOR) continue;
                forge::Entity g = scene.create("lantern");
                auto& t = scene.registry().get<forge::Transform>(g);
                t.pos = {(float)x * TILE + TILE * 0.5f, (float)y * TILE + TILE * 0.5f};
                forge::Sprite sp;
                sp.texture = &tileset;
                sp.src = {T_TRIM * TILE, 0, TILE, TILE};
                sp.size = {18, 18};
                sp.color = kGem;
                scene.registry().emplace<forge::Sprite>(g, sp);
                gems.push_back(g);
            }
        }
        message = "Find the lanterns";
        messageTimer = 3.0f;
    }

    void refreshGems() {
        gems.clear();
        scene.registry().view<forge::Tag>([&](forge::Entity e, forge::Tag& t) {
            if (t.name == "lantern") gems.push_back(e);
        });
    }

    // ------------------------------------------------------------- update
    void update(float dt) {
        lastDt = dt;
        time += dt;
        bob += dt;
        if (messageTimer > 0) messageTimer -= dt;
        if (streakTimer > 0) {
            streakTimer -= dt;
            if (streakTimer <= 0) streak = 0;
        }

        auto& in = engine->input();
        if (in.pressed(forge::Key::Q)) engine->quit();
        if (in.pressed(forge::Key::F1) && overlayReady) overlay.handleInput(in);
        if (in.pressed(forge::Key::F3)) wireframe = !wireframe;
        if (in.pressed(forge::Key::H)) showHelp = !showHelp;
        if (in.pressed(forge::Key::F5)) save();
        if (in.pressed(forge::Key::F9)) load();
        if (in.pressed(forge::Key::F2)) {
            loadScript();
            message = scriptFromFile ? "Script reloaded from " + scriptPath
                                     : "Script reloaded (builtin)";
            messageTimer = 2.0f;
        }

        // Movement: action remaps + stick + touch drag.
        Vec2 move{0, 0};
        if (in.actionDown("left")) move.x -= 1;
        if (in.actionDown("right")) move.x += 1;
        if (in.actionDown("up")) move.y += 1;
        if (in.actionDown("down")) move.y -= 1;
        Vec2 stick = in.moveAxis(0);
        move += stick;
        if (!in.touches().empty()) {
            Vec2 drag = in.touchDrag(in.touches().front().id);
            if (drag.lengthSq() > 64.0f) move += drag.normalized();
        }
        if (move.lengthSq() > 1.0f) move = move.normalized();
        if (autopilot && player != forge::kNullEntity && !gems.empty()) {
            Vec2 dir = autopilotDir(scene.registry().get<forge::Transform>(player).pos);
            if (dir.lengthSq() > 1.0f) move = dir.normalized();
        }
        movePlayer(move * PLAYER_SPEED * dt);

        // Lantern pickup: AABB overlap with a small circle-as-box.
        if (player != forge::kNullEntity) {
            const Vec2 p = scene.registry().get<forge::Transform>(player).pos;
            for (size_t i = 0; i < gems.size();) {
                forge::Entity g = gems[i];
                if (!scene.registry().alive(g)) { gems.erase(gems.begin() + (long)i); continue; }
                const Vec2 gp = scene.registry().get<forge::Transform>(g).pos;
                if (std::fabs(p.x - gp.x) < 22.0f && std::fabs(p.y - gp.y) < 22.0f) {
                    // collect() refreshes `gems` from the registry; re-check the
                    // same index instead of erasing (erasing here used to drop a
                    // *different*, still-alive lantern from the tracking list).
                    collect(g);
                    continue;
                }
                ++i;
            }
        }

        scene.update(dt);
        if (lightingReady) lighting.update(dt);
        if (postReady) post.setTime(time);

        if (!std::isfinite(cam.position.x) || !std::isfinite(cam.position.y)) healthy = false;
    }

    // Autopilot steering: breadth-first search over walkable tiles from the
    // player's tile to the nearest lantern by path length, then head for the
    // next tile on that path (steering straight at the lantern pins the player
    // against the raised wall). Zero when no lantern is reachable.
    Vec2 autopilotDir(Vec2 p) const {
        if (p.x < 0 || p.y < 0 || p.x >= VIEW_W || p.y >= VIEW_H) return {0, 0};
        auto index = [](int x, int y) { return y * MAP_W + x; };
        auto walkable = [&](int x, int y) {
            return x >= 0 && y >= 0 && x < MAP_W && y < MAP_H && !map.isSolid(map.tile(0, x, y));
        };
        std::vector<int> lanternAt(MAP_W * MAP_H, -1); // tile -> index into gems
        for (size_t i = 0; i < gems.size(); ++i) {
            if (!scene.registry().alive(gems[i])) continue;
            const Vec2 gp = scene.registry().get<forge::Transform>(gems[i]).pos;
            lanternAt[index((int)(gp.x / TILE), (int)(gp.y / TILE))] = (int)i;
        }
        const int start = index((int)(p.x / TILE), (int)(p.y / TILE));
        std::vector<int> parent(MAP_W * MAP_H, -1);
        std::vector<int> queue{start};
        parent[start] = start;
        for (size_t head = 0; head < queue.size(); ++head) {
            const int cur = queue[head];
            if (lanternAt[cur] >= 0) {
                int step = cur; // walk back to the tile right after the start
                while (parent[step] != start && step != start) step = parent[step];
                const Vec2 target =
                    step == start
                        ? scene.registry().get<forge::Transform>(gems[(size_t)lanternAt[cur]]).pos
                        : Vec2{(step % MAP_W + 0.5f) * TILE, (step / MAP_W + 0.5f) * TILE};
                return target - p;
            }
            const int cx = cur % MAP_W, cy = cur / MAP_W;
            const int nx[4] = {cx + 1, cx - 1, cx, cx}, ny[4] = {cy, cy, cy + 1, cy - 1};
            for (int k = 0; k < 4; ++k) {
                if (!walkable(nx[k], ny[k]) || parent[index(nx[k], ny[k])] >= 0) continue;
                parent[index(nx[k], ny[k])] = cur;
                queue.push_back(index(nx[k], ny[k]));
            }
        }
        return {0, 0};
    }

    void movePlayer(Vec2 delta) {
        if (player == forge::kNullEntity) return;
        auto& t = scene.registry().get<forge::Transform>(player);
        // Axis-separated tile collision so sliding along walls feels right.
        Rect box = Rect::fromCenter(t.pos + Vec2{delta.x, 0}, {PLAYER_R * 2, PLAYER_R * 2});
        Vec2 push;
        if (map.collideRect(box, &push)) t.pos.x += push.x;
        else t.pos.x += delta.x;
        box = Rect::fromCenter(t.pos + Vec2{0, delta.y}, {PLAYER_R * 2, PLAYER_R * 2});
        if (map.collideRect(box, &push)) t.pos.y += push.y;
        else t.pos.y += delta.y;
        // Keep the camera on the player but inside the map.
        cam.position.x = forge::clamp(t.pos.x, VIEW_W * 0.5f, VIEW_W * 0.5f);
        cam.position.y = forge::clamp(t.pos.y, VIEW_H * 0.5f, VIEW_H * 0.5f);
    }

    void collect(forge::Entity g) {
        streak += 1;
        streakTimer = 2.5f;
        auto v = vm.call("on_collect", forge::script::args((double)streak));
        int add = (int)v.number(10.0);
        if (!vm.error().ok()) add = 10;
        score += add;
        auto& t = scene.registry().get<forge::Transform>(g);
        // little burst: reuse particles-free pop via a short-lived ring
        (void)t;
        scene.destroy(g);
        refreshGems();
        if (gems.empty()) {
            message = "All lanterns lit!";
            messageTimer = 5.0f;
        }
    }

    // ------------------------------------------------------------- save/load
    void save() {
        if (forge::saveScene(scene, "lantern-save.json", &engine->assets())) {
            // Save score/script state alongside the entities.
            forge::json::Value meta = forge::json::Value::object();
            meta.set("score", forge::json::Value(score));
            meta.set("streak", forge::json::Value(streak));
            forge::json::writeFile("lantern-save.meta.json", meta, true);
            message = "Saved to lantern-save.json";
        } else {
            message = "Save failed (read-only folder?)";
        }
        messageTimer = 2.5f;
    }

    void load() {
        if (!forge::loadScene(scene, "lantern-save.json", &engine->assets())) {
            message = "No save found";
            messageTimer = 2.5f;
            return;
        }
        player = scene.findByTag("player");
        refreshGems();
        forge::json::ParseError err;
        forge::json::Value meta = forge::json::parseFile("lantern-save.meta.json", &err);
        if (err.ok()) {
            score = meta["score"].asInt(0);
            streak = meta["streak"].asInt(0);
        }
        vm.call("reset");
        for (size_t i = 0; i < gems.size(); ++i) vm.call("on_collect", forge::script::args(0.0));
        message = "Loaded lantern-save.json";
        messageTimer = 2.5f;
    }

    // ------------------------------------------------------------- render
    void renderTo(forge::Framebuffer* fb) {
        forge::IVec2 vp = fb ? forge::IVec2{fb->width(), fb->height()} : viewport;
        // With post enabled the world is composed offscreen, then the effect
        // is applied while blitting to the real target. The HUD/overlay are
        // drawn afterwards so UI text stays crisp (no vignette on it).
        forge::Framebuffer* worldTarget = fb;
        if (postReady) {
            if (!sceneFbo.valid() || sceneFbo.width() != vp.x || sceneFbo.height() != vp.y) {
                if (!sceneFbo.create(vp.x, vp.y)) { postReady = false; }
            }
            if (postReady) worldTarget = &sceneFbo;
        }
        if (worldTarget) worldTarget->bind();
        else forge::Framebuffer::unbind(vp.x, vp.y);
        forge::gl::glViewport(0, 0, vp.x, vp.y);
        forge::gl::glClearColor(kBg.r, kBg.g, kBg.b, 1.0f);
        forge::gl::glClear(GL_COLOR_BUFFER_BIT);

        fitCamera(vp);
        auto& b = engine->batch();
        b.begin(cam.viewProj(vp.x, vp.y));

        map.draw(b, {0, 0}, Rect{-1000, -1000, VIEW_W + 2000, VIEW_H + 2000});
        drawLanterns(b);
        drawPlayer(b);
        if (wireframe) map.drawAll(b, {0, 0});
        b.end();

        // Lightmap composite: lights are in world space, so the same camera.
        if (lightingReady) {
            updateLights();
            lighting.render(b, cam, vp, worldTarget);
        }

        // Post: scene texture -> real target (screen or the capture FBO).
        if (postReady && worldTarget == &sceneFbo) {
            if (fb) fb->bind();
            else forge::Framebuffer::unbind(vp.x, vp.y);
            forge::gl::glViewport(0, 0, vp.x, vp.y);
            sceneView.adoptRaw(sceneFbo.texture(), vp.x, vp.y, false);
            post.apply(sceneView, vp);
        }

        // HUD in screen space: the same centered, Y-up ortho the other
        // examples use, so font.draw(pos) is the top-left of the text block.
        forge::Mat4 hud = forge::Mat4::ortho(-vp.x * 0.5f, vp.x * 0.5f,
                                             -vp.y * 0.5f, vp.y * 0.5f);
        b.begin(hud);
        drawHud(b, vp);
        if (overlayReady) {
            overlay.update(lastDt, forge::Profiler::instance(), b);
            // The overlay expects a centered, Y-up space: give it one.
            b.end();
            b.begin(forge::Mat4::ortho(-vp.x * 0.5f, vp.x * 0.5f,
                                       -vp.y * 0.5f, vp.y * 0.5f));
            overlay.draw(b, vp, &scene, &scene.physics());
        }
        b.end();
        if (fb) forge::Framebuffer::unbind(viewport.x, viewport.y);
    }

    void fitCamera(forge::IVec2 vp) {
        // Letterbox the fixed 960x576 world into any window size.
        float sx = (float)vp.x / VIEW_W, sy = (float)vp.y / VIEW_H;
        cam.zoom = std::min(sx, sy);
        cam.position = {VIEW_W * 0.5f, VIEW_H * 0.5f};
    }

    void drawLanterns(forge::SpriteBatch& b) {
        float pulse = 0.85f + 0.15f * std::sin(bob * 3.0f);
        for (auto g : gems) {
            if (!scene.registry().alive(g)) continue;
            const auto& t = scene.registry().get<forge::Transform>(g);
            b.drawCircle({t.pos.x, t.pos.y + std::sin(bob * 2.0f + t.pos.x) * 2.0f}, 13.0f,
                         Color{kGem.r, kGem.g, kGem.b, 0.22f * pulse});
            b.drawRect(Rect::fromCenter(t.pos, {12, 12}), Color{kGem.r, kGem.g, kGem.b, pulse});
            b.drawRect(Rect::fromCenter(t.pos, {5, 5}), Color::white());
        }
    }

    void drawPlayer(forge::SpriteBatch& b) {
        if (player == forge::kNullEntity || !scene.registry().alive(player)) return;
        const auto& t = scene.registry().get<forge::Transform>(player);
        b.drawCircle(t.pos, PLAYER_R + 3.0f, Color{kLantern.r, kLantern.g, kLantern.b, 0.18f});
        b.drawRect(Rect::fromCenter(t.pos, {PLAYER_R * 1.6f, PLAYER_R * 1.8f}),
                   Color::fromHex(0xF2E7D8FF));
        b.drawRect(Rect::fromCenter(t.pos + Vec2{0, -6}, {PLAYER_R * 1.6f, 4}),
                   Color::fromHex(0x8A6A3AFF));
    }

    void updateLights() {
        lighting.clearLights();
        forge::Light2D lantern;
        lantern.pos = player != forge::kNullEntity
            ? scene.registry().get<forge::Transform>(player).pos
            : Vec2{VIEW_W * 0.5f, VIEW_H * 0.5f};
        lantern.radius = (float)vm.call("light_radius", forge::script::args(210.0)).number(210.0);
        lantern.color = kLantern;
        lantern.intensity = 1.0f;
        lantern.flickerAmount = 0.04f;
        lantern.flickerSpeed = 9.0f;
        lantern.flickerPhase = 0.7f;
        lighting.addLight(lantern);
        for (auto g : gems) {
            if (!scene.registry().alive(g)) continue;
            forge::Light2D gl;
            gl.pos = scene.registry().get<forge::Transform>(g).pos;
            gl.radius = 92.0f;
            gl.color = kGem;
            gl.intensity = 0.85f;
            gl.flickerAmount = 0.10f;
            gl.flickerSpeed = 2.6f;
            gl.flickerPhase = gl.pos.x * 0.013f;
            lighting.addLight(gl);
        }
    }

    void drawHud(forge::SpriteBatch& b, forge::IVec2 vp) {
        if (!haveFont) return;
        const float left = -(float)vp.x * 0.5f + 24.0f;
        const float top = (float)vp.y * 0.5f - 14.0f;
        const float bottom = -(float)vp.y * 0.5f + 34.0f;
        char line[128];
        std::snprintf(line, sizeof(line), "SCORE %04d    LANTERNS %d", score, (int)gems.size());
        fontBig.draw(b, line, {left, top}, 1.0f, kHud);

        // Objective comes from the script (hot-reloadable).
        auto obj = vm.call("objective", forge::script::args((double)gems.size()));
        std::string text = vm.error().ok() ? obj.string() : "script error";
        font.draw(b, text, {left + 2, top - 48}, 1.0f, kHudDim);

        if (messageTimer > 0) {
            float a = forge::clamp01(messageTimer);
            font.draw(b, message, {left + 2, bottom + 46}, 1.0f,
                      Color{kAccent.r, kAccent.g, kAccent.b, a});
        }
        if (showHelp) {
            font.draw(b, "WASD/arrows move  ·  F5 save  ·  F9 load  ·  F2 reload script",
                      {left + 2, bottom + 22}, 0.85f, kHudDim);
            font.draw(b, "F1 overlay  ·  F3 tile grid  ·  H hide help  ·  Q quit",
                      {left + 2, bottom}, 0.85f, kHudDim);
        }
        if (!scriptFromFile)
            font.draw(b, "script: builtin (create assets/lantern.fgs to hot-reload)",
                      {left + 2, bottom - 24}, 0.8f, Color{kHudDim.r, kHudDim.g, kHudDim.b, 0.8f});
    }
};

// ---------------------------------------------------------------- capture
bool saveScreenshot(const std::string& path, forge::IVec2 vp) {
    std::vector<uint8_t> px((size_t)vp.x * (size_t)vp.y * 4);
    forge::gl::glReadPixels(0, 0, vp.x, vp.y, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    std::vector<uint8_t> flipped(px.size());
    const size_t row = (size_t)vp.x * 4;
    for (int y = 0; y < vp.y; ++y)
        std::memcpy(&flipped[(size_t)y * row], &px[(size_t)(vp.y - 1 - y) * row], row);
    return stbi_write_png(path.c_str(), vp.x, vp.y, 4, flipped.data(), (int)row) != 0;
}

struct Args {
    std::string shot;
    std::string record;
    int frames = 0;
    int width = 960, height = 576;
    bool help = false;
    bool noSave = false;
    bool selftest = false;
    bool overlay = false;
};

Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (arg == "--shot") { if (const char* v = next()) a.shot = v; }
        else if (arg == "--record") { if (const char* v = next()) a.record = v; }
        else if (arg == "--frames") { if (const char* v = next()) a.frames = std::atoi(v); }
        else if (arg == "--size") {
            if (const char* v = next()) std::sscanf(v, "%dx%d", &a.width, &a.height);
        } else if (arg == "--no-save") a.noSave = true;
        else if (arg == "--selftest") a.selftest = true;
        else if (arg == "--overlay") a.overlay = true;
        else if (arg == "-h" || arg == "--help") a.help = true;
    }
    if (a.width < 64) a.width = 64;
    if (a.height < 64) a.height = 64;
    return a;
}

void printUsage() {
    std::printf(
        "10_lantern — Forge integration example\n"
        "  WASD/arrows move · F5 save · F9 load · F2 reload script\n"
        "  F1 overlay · F3 tile grid · H help · Q/ESC quit\n"
        "Options:\n"
        "  --shot FILE   simulate N frames, render offscreen, save PNG, exit\n"
        "  --record FILE record N frames of the autopilot to an MP4 (ffmpeg)\n"
        "  --frames N    frames to simulate before the shot / record / CI exit\n"
        "  --size WxH    capture size (default 960x576)\n"
        "  --no-save     do not write the autosave files (CI)\n"
        "  --selftest    run the save/load round-trip check and exit\n"
        "  --overlay     draw the debug overlay into the capture\n");
}

} // namespace

int main(int argc, char** argv) {
    const Args args = parseArgs(argc, argv);
    if (args.help) { printUsage(); return 0; }

    forge::EngineConfig cfg;
    cfg.window.title = "Forge 10_lantern";
    cfg.window.width = 1280;
    cfg.window.height = 768;
    cfg.window.vsync = args.shot.empty() && args.record.empty();
    cfg.fixedStep = 1.0f / 60.0f;
#if FORGE_PLATFORM_WEB
    // The drawing buffer takes the canvas's CSS size once and then stays put
    // (not resizable), so the lightmap and post targets keep one size.
    cfg.window.resizable = false;
    // Emscripten takes the frame loop over by unwinding main()'s stack, and
    // with wasm exceptions on that unwind runs the destructors of main()'s
    // locals (a unique_ptr owner would free the game under the loop). Statics
    // outlive main and are never destroyed: the web runtime never exits.
    static forge::Engine engine(cfg);
    static Game game;
#else
    forge::Engine engine(cfg);
    Game game;
#endif
    if (!engine.valid()) return 1;

    if (!game.init(engine)) return 1;
    game.viewport = engine.viewport();
    // The overlay is a debug tool: off during normal play (F1 toggles), and
    // opt-in for captures so the default shot shows the game, not the panel.
    if (game.overlayReady) game.overlay.setEnabled(args.overlay);

    if (args.selftest) {
        // Headless integration check for the save/load path (F5/F9) that the
        // smoke run cannot press: simulate, save, mutate, load, compare.
        game.autopilot = true;
        for (int i = 0; i < 90; ++i) game.update(1.0f / 60.0f);
        const int scoreBefore = game.score;
        const size_t gemsBefore = game.gems.size();
        const bool saved = (game.save(), game.message == "Saved to lantern-save.json");
        game.score += 999;
        for (int i = 0; i < 2 && !game.gems.empty(); ++i) {
            game.scene.destroy(game.gems.back());
            game.gems.pop_back();
        }
        const size_t gemsMutated = game.gems.size();
        game.load();
        const bool ok = saved && game.score == scoreBefore &&
                        game.gems.size() == gemsBefore &&
                        gemsMutated + 2 == gemsBefore && game.healthy;
        std::printf("selftest: save/load %s (score %d -> %d, gems %zu -> %zu -> %zu)\n",
                    ok ? "OK" : "FAILED", scoreBefore, game.score, gemsBefore,
                    gemsMutated, game.gems.size());
        std::remove("lantern-save.json");
        std::remove("lantern-save.meta.json");
        return ok ? 0 : 1;
    }

#if !FORGE_PLATFORM_WEB
    if (!args.record.empty()) {
        // Video of the autopilot lighting every lantern: one fixed step per
        // frame, rendered offscreen at --size (pass the 1280x768 window size
        // for the full view; the default is the 960x576 world).
        game.autopilot = true;
        const bool ok = clip::recordClip(
            args.record, args.width, args.height, args.frames > 0 ? args.frames : 900,
            [&] { game.update(1.0f / 60.0f); },
            [&](forge::Framebuffer* fb) { game.renderTo(fb); }, engine);
        return ok && game.healthy ? 0 : 1;
    }
#endif

    if (!args.shot.empty()) {
        game.autopilot = true;
        const int frames = args.frames > 0 ? args.frames : 300;
        // Deterministic path: no wall clock, no real input. Walk right for a
        // while so the shot shows lit lanterns and a moving player.
        for (int i = 0; i < frames; ++i) {
            // This loop bypasses Engine::runFixed, so open the same profiler
            // zones it would: the overlay's update/render ms stay meaningful
            // in the capture.
            forge::Profiler::instance().beginFrame();
            {
                FORGE_ZONE("update");
                game.update(1.0f / 60.0f);
            }
            {
                FORGE_ZONE("render");
                if (game.overlayReady)
                    game.overlay.update(1.0f / 60.0f, forge::Profiler::instance(),
                                        engine.batch());
            }
            forge::Profiler::instance().endFrame();
        }
        forge::Framebuffer fbo;
        const bool offscreen = fbo.create(args.width, args.height);
        game.renderTo(offscreen ? &fbo : nullptr);
        forge::IVec2 size = offscreen ? forge::IVec2{args.width, args.height} : engine.viewport();
        if (offscreen) fbo.bind();
        const bool ok = saveScreenshot(args.shot, size);
        if (offscreen) forge::Framebuffer::unbind(engine.viewport().x, engine.viewport().y);
        std::printf("shot: %s (%dx%d, %d frames) score=%d lanterns=%d sane=%d\n",
                    args.shot.c_str(), size.x, size.y, frames, game.score,
                    (int)game.gems.size(), game.healthy);
        return ok && game.healthy ? 0 : 1;
    }

#if FORGE_PLATFORM_WEB
    // Runs until the tab closes; saves (F5) live in the page's in-memory FS.
    // Nothing to capture: the loop touches only the statics, and main()'s
    // locals are gone by frame one.
    engine.runFixed([](float dt) { game.update(dt); }, [] { game.renderTo(nullptr); });
#else
    const bool ci = std::getenv("CI") != nullptr;
    const int ciFrames = args.frames > 0 ? args.frames : 240;
    game.autopilot = ci;
    int frame = 0;
    engine.runFixed(
        [&](float dt) {
            game.update(dt);
            if (ci && ++frame >= ciFrames) engine.quit();
        },
        [&]() {
            game.viewport = engine.viewport(); // follow window resizes
            game.renderTo(nullptr);
        });

    if (!args.noSave && !ci) {
        // Best-effort autosave on a clean exit; never fatal.
        game.save();
    }
    if (ci) std::printf("CI ok: %d frames, score=%d lanterns=%d\n", frame, game.score,
                        (int)game.gems.size());
#endif
    return game.healthy ? 0 : 1;
}
