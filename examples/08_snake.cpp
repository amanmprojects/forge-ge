// 08_snake — classic Snake, built on Forge.
//
//   move    WASD · arrows · gamepad dpad/stick
//   start   SPACE · click · gamepad A       confirm on menus
//   pause   ESC          restart  R          quit  Q
//
// Grid-based with smooth interpolation: the snake glides cell to cell at a
// tick that accelerates as you eat. Apples pay 10; every 5th apple drops a
// golden bonus worth 50 that expires if you dawdle. Walls and your own tail
// kill. Everything is procedural: the checkerboard, snake and food are
// batched white-quad shapes and TTF text, SFX are synthesized WAVs at
// startup, eats/deaths are ParticleSystem bursts with camera trauma.
// The title screen runs a bot game behind the menu.
//
// Headless capture (docs/CI): runs N fixed steps with a scripted autopilot,
// writes the final frame to a PNG and exits.
//   ./08_snake --shot out.png --frames 900 [--title] [--no-ai] [--sloppy]
// CI=1 ./08_snake [--frames 240]          # autopilot smoke run, then exit 0
#include <forge/forge.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(__GNUC__)
// Vendored single-header lib: silence its warnings in this TU (the example
// itself is built with the project's -Wall -Wextra -Wpedantic).
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
using forge::IVec2;
using forge::Rect;
using forge::Vec2;

// ------------------------------------------------------------------ layout
// Fixed 640x480 logical playfield (4:3), letterboxed into any window by a
// fitted Camera2D. All coordinates below are playfield units, Y-up.
// A 20 px wall band frames the playable grid: 30x20 cells of 20 px.
constexpr float FIELD_W = 640.0f, FIELD_H = 480.0f;
constexpr float HUD_H = 40.0f;              // top bar (doubles as the top wall)
constexpr float PLAY_H = FIELD_H - HUD_H;   // 440: wall + grid + wall
constexpr float WALL = 20.0f;               // wall band thickness
constexpr float CELL = 20.0f;               // grid cell size
constexpr int GRID_W = 30, GRID_H = 20;     // playable cells (600 total)
static_assert(WALL * 2 + CELL * GRID_W == FIELD_W, "grid must fill the field");
static_assert(WALL * 2 + CELL * GRID_H == PLAY_H, "grid must fill the field");

constexpr int START_LEN = 4;
constexpr int APPLE_SCORE = 10, GOLD_SCORE = 50, PERFECT_BONUS = 100;
constexpr int GOLD_EVERY = 5;               // every Nth apple drops a bonus
constexpr float GOLD_LIFE = 6.5f;           // seconds before the bonus rots
constexpr float STEP_BASE = 0.155f;         // s per cell at length 4
constexpr float STEP_MIN = 0.072f;          // speed floor
constexpr float STEP_PER_APPLE = 0.0015f;   // ramp per apple eaten
constexpr float DEATH_HOLD = 1.15f;         // death animation -> game over

// ------------------------------------------------------------------ palette
const Color kPanel = Color::fromHex(0x0B130EFF);
const Color kPanelAlt = Color::fromHex(0x0E1712FF);   // checkerboard
const Color kWall = Color::fromHex(0x1A2A20FF);
const Color kWallAlt = Color::fromHex(0x152219FF);    // wall block pattern
const Color kWallEdge = Color::fromHex(0x3A6B46FF);
const Color kAccent = Color::fromHex(0x2A4A32FF);
const Color kHudBar = Color::fromHex(0x0D1410FF);
const Color kSnakeHead = Color::fromHex(0xA7F582FF);
const Color kSnakeTail = Color::fromHex(0x2C7A3CFF);
const Color kApple = Color::fromHex(0xE8483FFF);
const Color kAppleDark = Color::fromHex(0x932C27FF);
const Color kStem = Color::fromHex(0x8A6238FF);
const Color kLeaf = Color::fromHex(0x6FBF5AFF);
const Color kGold = Color::fromHex(0xFFD166FF);
const Color kGoldDeep = Color::fromHex(0xB8862EFF);
const Color kDanger = Color::fromHex(0xFF5D4DFF);

static Color shade(Color c, float f) {
    return {std::min(1.0f, c.r * f), std::min(1.0f, c.g * f),
            std::min(1.0f, c.b * f), c.a};
}
static Color withA(Color c, float a) { return {c.r, c.g, c.b, a}; }

// ------------------------------------------------------------ SFX synthesis
// Tiny chiptune synth: each SFX is a chain of notes rendered into an in-memory
// 16-bit mono WAV (the same loadMemory path a decoded asset file would take).
struct Note {
    float f0, f1;   // start/end frequency (Hz), swept linearly
    float dur;      // seconds
    float vol;      // 0..1
    int wave;       // 0 sine, 1 square, 2 triangle
    float noise;    // 0..1 noise blend
};

std::vector<uint8_t> synthWav(const std::vector<Note>& notes, int rate = 22050) {
    std::vector<int16_t> pcm;
    float phase = 0.0f;
    forge::Random rng{0x51EED5};
    for (const Note& n : notes) {
        int frames = std::max(1, (int)(n.dur * rate));
        for (int i = 0; i < frames; ++i) {
            float u = (float)i / (float)frames;
            float f = forge::lerp(n.f0, n.f1, u);
            phase += f / (float)rate;
            float s;
            if (n.wave == 1) s = std::sin(forge::TAU * phase) >= 0.0f ? 1.0f : -1.0f;
            else if (n.wave == 2) s = std::asin(std::sin(forge::TAU * phase)) * (2.0f / forge::PI);
            else s = std::sin(forge::TAU * phase);
            float noise = rng.nextFloat() * 2.0f - 1.0f;
            s = s * (1.0f - n.noise) + noise * n.noise;
            // 2.5 ms attack + exponential decay keeps blips punchy, not clicky
            float env = std::min(1.0f, (float)i / (0.0025f * (float)rate)) *
                        std::exp(-3.2f * u);
            pcm.push_back((int16_t)(forge::clamp(s * env * n.vol, -1.0f, 1.0f) * 32767.0f));
        }
    }
    const int bytes = (int)pcm.size() * 2;
    std::vector<uint8_t> wav((size_t)(44 + bytes));
    auto w32 = [&](int o, uint32_t v) { std::memcpy(&wav[(size_t)o], &v, 4); };
    auto w16 = [&](int o, uint16_t v) { std::memcpy(&wav[(size_t)o], &v, 2); };
    std::memcpy(&wav[0], "RIFF", 4);
    w32(4, (uint32_t)(36 + bytes));
    std::memcpy(&wav[8], "WAVEfmt ", 8);
    w32(16, 16); w16(20, 1); w16(22, 1);
    w32(24, (uint32_t)rate); w32(28, (uint32_t)rate * 2); w16(32, 2); w16(34, 16);
    std::memcpy(&wav[36], "data", 4);
    w32(40, (uint32_t)bytes);
    std::memcpy(&wav[44], pcm.data(), (size_t)bytes);
    return wav;
}

// ------------------------------------------------------------------- game
struct Popup {  // floating "+10" score text
    Vec2 pos;
    int value = 0;
    bool gold = false;
    float life = 0.0f;
};

class Snake {
public:
    enum class Screen { Title, Playing, GameOver };

    void init(forge::Engine& engine) {
        engine_ = &engine;
        fitCamera(engine.viewport());

        haveBig_ = loadFont(fontBig_, 54.0f, "FORGE_FONT_TITLE");
        loadFont(font_, 24.0f, "FORGE_FONT");
        bigFont_ = haveBig_ ? &fontBig_ : &font_;
        bigScale_ = haveBig_ ? 1.45f : 3.2f;

        initAudio();
        initParticles();
        startRun();
        screen_ = Screen::Title;
    }

    void setAutoplay(bool on) { autoplay_ = on; }
    void setSloppyBot(bool on) { sloppy_ = on; }
    void setSeed(uint64_t s) { rng_.seed(s); }

    int score() const { return score_; }
    int length() const { return (int)body_.size(); }
    int apples() const { return applesEaten_; }
    bool perfect() const { return perfect_; }

    // Invariant check for CI: a healthy frame never has NaN timers and the
    // snake/food always live inside the grid. --shot fails (exit 1) if this
    // trips.
    bool healthy() const {
        if (std::isnan(interp_) || std::isnan(stepTimer_) || std::isnan(deathTimer_))
            return false;
        if (body_.empty()) return false;
        if (body_.size() > (size_t)(GRID_W * GRID_H)) return false;
        for (const IVec2& c : body_)
            if (!inBounds(c)) return false;
        if (!inBounds(food_)) return false;
        if (goldenActive_ && !inBounds(golden_)) return false;
        if (interp_ < -0.001f || interp_ > 1.001f) return false;
        if (stepInterval() < STEP_MIN - 1e-4f) return false;
        return true;
    }

    // Fresh run: starts on the title screen; startGame() flips to playing.
    void startRun() {
        body_.clear();
        const int cy = GRID_H / 2;
        for (int i = 0; i < START_LEN; ++i)
            body_.push_back({GRID_W / 2 + 1 - i, cy});
        from_ = body_;
        dir_ = {1, 0};
        turnQueue_.clear();
        stickDir_ = {0, 0};
        growPending_ = 0;
        score_ = 0;
        applesEaten_ = 0;
        runTime_ = 0.0f;
        newHigh_ = false;
        death_ = false;
        deathTimer_ = 0.0f;
        perfect_ = false;
        stepTimer_ = 0.0f;
        interp_ = 0.0f;
        headFlash_ = 0.0f;
        goldenActive_ = false;
        goldenTimer_ = 0.0f;
        paused_ = false;
        popups_.clear();
        for (auto& fx : fx_) fx.clear();
        spawnCell(food_);
    }

    void startGame() {
        startRun();
        screen_ = Screen::Playing;
        titleTimer_ = 0.0f;
        playSfx(sfx_.start, 0.8f);
    }

    // ------------------------------------------------------------ update
    void update(float dt) {
        uiTime_ += dt;  // keeps menus blinking even while paused
        auto& in = engine_->input();
        fitCamera(engine_->viewport());

        if (in.pressed(forge::Key::Q)) { engine_->quit(); return; }

        if (screen_ == Screen::Playing && !death_ &&
            in.pressed(forge::Key::Escape)) {
            paused_ = !paused_;
            playSfx(sfx_.pause, 0.6f);
        }
        if (in.pressed(forge::Key::R) && screen_ != Screen::Title) {
            startGame();
            return;
        }

        const bool confirm = !autoplay_ &&
            (in.pressed(forge::Key::Space) || in.pressed(forge::Key::Enter) ||
             in.mousePressed(forge::MouseButton::Left) ||
             padPressedAny(in, forge::GamepadButton::A));

        if (!paused_) {
            updateEffects(dt);
            if (screen_ == Screen::Title) {
                titleTimer_ += dt;
                if (death_) {
                    if (deathTimer_ > 1.2f) startRun();  // keep the demo alive
                } else {
                    simulate(dt, true);  // bot plays behind the menu
                }
                if (confirm || (autoplay_ && titleTimer_ > 0.6f)) startGame();
            } else if (screen_ == Screen::Playing) {
                if (death_) {
                    deathTimer_ += dt;
                    if (deathTimer_ > DEATH_HOLD) {
                        screen_ = Screen::GameOver;
                        overTimer_ = 0.0f;
                        playSfx(sfx_.over, 0.9f);
                    }
                } else {
                    queueTurns(in);
                    simulate(dt, autoplay_);
                }
            } else {  // GameOver
                overTimer_ += dt;
                if (confirm || (autoplay_ && overTimer_ > 1.0f)) startGame();
            }
        }
        cam.update(dt);
    }

    // ------------------------------------------------------------ render
    void render() { renderTo(nullptr); }

    // fb == nullptr renders to the window; a target renders offscreen (the
    // --shot capture path, so PNGs are independent of window-manager sizing).
    void renderTo(forge::Framebuffer* fb) {
        forge::IVec2 vp = fb ? forge::IVec2{fb->width(), fb->height()}
                             : engine_->viewport();
        if (fb) fb->bind();
        forge::gl::glViewport(0, 0, vp.x, vp.y);
        forge::gl::glClearColor(0.027f, 0.043f, 0.031f, 1.0f);
        forge::gl::glClear(GL_COLOR_BUFFER_BIT);
        fitCamera(vp);
        auto& b = engine_->batch();
        b.begin(cam.viewProj(vp.x, vp.y));

        drawField(b);
        drawFood(b);
        drawSnake(b);
        for (const auto& fx : fx_) fx.draw(b);
        drawPopups(b);
        drawHud(b);
        drawOverlays(b);

        b.end();
        if (fb) forge::Framebuffer::unbind(engine_->viewport().x, engine_->viewport().y);
    }

private:
    // ------------------------------------------------------------ setup
    static bool fileExists(const std::string& p) {
        if (FILE* f = std::fopen(p.c_str(), "rb")) { std::fclose(f); return true; }
        return false;
    }

    static bool loadFont(forge::Font& f, float px, const char* envKey) {
        std::vector<std::string> paths;
        if (const char* env = std::getenv(envKey)) paths.push_back(env);
        const char* system[] = {
            "/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf",
            "/usr/share/fonts/Adwaita/AdwaitaMono-Bold.ttf",
            "/usr/share/fonts/liberation/LiberationMono-Bold.ttf",
            "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/noto/NotoSansMono-Bold.ttf",
            "/usr/share/fonts/noto/NotoSans-Bold.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
            "/usr/share/fonts/dejavu/DejaVuSansMono-Bold.ttf",
            "/System/Library/Fonts/Menlo.ttc",
            "C:/Windows/Fonts/consolab.ttf",
        };
        for (const char* p : system) paths.emplace_back(p);
        for (const std::string& p : paths) {
            if (fileExists(p) && f.loadFile(p, px)) return true;
        }
        return false;  // text silently disabled; the game is still playable
    }

    void initAudio() {
        auto& a = engine_->audio();
        auto add = [&](const std::vector<Note>& notes) {
            auto wav = synthWav(notes);
            return a.loadMemory(wav.data(), wav.size());
        };
        sfx_.eat = add({{540, 720, 0.045f, 0.30f, 1, 0.0f},
                        {760, 990, 0.050f, 0.26f, 1, 0.0f}});
        sfx_.gold = add({{880, 880, 0.050f, 0.28f, 2, 0.0f},
                         {1108, 1108, 0.050f, 0.28f, 2, 0.0f},
                         {1318, 1318, 0.055f, 0.30f, 2, 0.0f},
                         {1760, 1760, 0.130f, 0.30f, 2, 0.0f}});
        sfx_.spawn = add({{1180, 1560, 0.070f, 0.20f, 2, 0.0f}});
        sfx_.expire = add({{700, 430, 0.110f, 0.18f, 2, 0.0f}});
        sfx_.die = add({{520, 92, 0.50f, 0.42f, 1, 0.12f},
                        {150, 70, 0.28f, 0.30f, 2, 0.0f}});
        sfx_.over = add({{330, 330, 0.140f, 0.32f, 1, 0.0f},
                         {262, 262, 0.140f, 0.32f, 1, 0.0f},
                         {196, 196, 0.140f, 0.32f, 1, 0.0f},
                         {147, 98, 0.400f, 0.34f, 1, 0.10f}});
        sfx_.start = add({{392, 392, 0.070f, 0.28f, 2, 0.0f},
                          {523, 523, 0.070f, 0.28f, 2, 0.0f},
                          {659, 659, 0.070f, 0.28f, 2, 0.0f},
                          {784, 784, 0.130f, 0.30f, 2, 0.0f}});
        sfx_.pause = add({{460, 460, 0.050f, 0.25f, 1, 0.0f}});
        sfx_.perfect = add({{659, 659, 0.10f, 0.30f, 2, 0.0f},
                            {784, 784, 0.10f, 0.30f, 2, 0.0f},
                            {988, 988, 0.10f, 0.30f, 2, 0.0f},
                            {1319, 1319, 0.10f, 0.32f, 2, 0.0f},
                            {1568, 1568, 0.30f, 0.32f, 2, 0.0f}});
    }

    static forge::EmitterConfig burstCfg(Color color, int max, uint64_t seed,
                                         float size, float life, float spread) {
        forge::EmitterConfig c;
        c.maxParticles = max;
        c.spawnRate = 0.0f;
        c.looping = false;
        c.vel = {0.0f, 20.0f};
        c.velVar = {spread, spread * 0.85f};
        c.accel = {0.0f, -900.0f};
        c.life = life;
        c.lifeVar = life * 0.45f;
        c.size = size;
        c.sizeVar = size * 0.45f;
        c.endSize = size * 0.12f;
        c.start = color;
        c.end = withA(color, 0.0f);
        c.damping = 1.2f;
        c.rotVelVar = 12.0f;
        c.seed = seed;
        return c;
    }

    void initParticles() {
        fxEat().configure(burstCfg(kApple, 192, 0x3000u, 5.0f, 0.45f, 190.0f));
        fxGold().configure(burstCfg(kGold, 256, 0x3100u, 5.5f, 0.70f, 230.0f));
        fxDie().configure(burstCfg(kDanger, 384, 0x3200u, 6.5f, 0.95f, 300.0f));
    }

    // ------------------------------------------------------------ audio
    float panFor(float x) const {
        return forge::clamp((x / FIELD_W - 0.5f) * 1.4f, -1.0f, 1.0f);
    }
    void playSfx(const forge::SoundHandle& h, float vol, float pan = 0.0f,
                 float pitch = 1.0f) {
        if (h.valid()) engine_->audio().play(h, vol, pan, pitch);
    }

    // ------------------------------------------------------------ geometry
    static bool inBounds(IVec2 c) {
        return c.x >= 0 && c.x < GRID_W && c.y >= 0 && c.y < GRID_H;
    }
    static IVec2 clampCell(IVec2 c) {
        return {std::clamp(c.x, 0, GRID_W - 1), std::clamp(c.y, 0, GRID_H - 1)};
    }
    static Vec2 cellCenter(IVec2 c) {
        return {WALL + ((float)c.x + 0.5f) * CELL,
                WALL + ((float)c.y + 0.5f) * CELL};
    }
    static bool isOpposite(IVec2 a, IVec2 b) {
        return a.x == -b.x && a.y == -b.y && !(a.x == 0 && a.y == 0);
    }
    static int manhattan(IVec2 a, IVec2 b) {
        return std::abs(a.x - b.x) + std::abs(a.y - b.y);
    }

    void fitCamera(forge::IVec2 vp) {
        cam.position = {FIELD_W * 0.5f, FIELD_H * 0.5f};
        cam.zoom = std::min((float)vp.x / FIELD_W, (float)vp.y / FIELD_H);
    }

    // ------------------------------------------------------------ rules
    float stepInterval() const {
        return std::max(STEP_MIN, STEP_BASE - (float)applesEaten_ * STEP_PER_APPLE);
    }

    IVec2 head() const { return body_.front(); }

    bool occupied(IVec2 c) const {
        for (const IVec2& b : body_)
            if (b == c) return true;
        if (goldenActive_ && golden_ == c) return true;
        return false;
    }

    bool spawnCell(IVec2& out) {
        for (int tries = 0; tries < 400; ++tries) {
            IVec2 c{(int)rng_.range(0.0f, (float)GRID_W),
                    (int)rng_.range(0.0f, (float)GRID_H)};
            if (inBounds(c) && !occupied(c)) { out = c; return true; }
        }
        for (int y = 0; y < GRID_H; ++y) {  // board nearly full: scan
            for (int x = 0; x < GRID_W; ++x) {
                IVec2 c{x, y};
                if (!occupied(c)) { out = c; return true; }
            }
        }
        return false;
    }

    void spawnGolden() {
        if (!spawnCell(golden_)) return;
        goldenActive_ = true;
        goldenTimer_ = GOLD_LIFE;
        playSfx(sfx_.spawn, 0.7f, panFor(cellCenter(golden_).x));
        fxGold().setPosition(cellCenter(golden_));
        fxGold().emit(10);
    }

    void tickGolden(float dt) {
        if (!goldenActive_) return;
        goldenTimer_ -= dt;
        if (goldenTimer_ <= 0.0f) {
            goldenActive_ = false;
            playSfx(sfx_.expire, 0.5f, panFor(cellCenter(golden_).x));
        }
    }

    // ------------------------------------------------------------ input
    void queueDir(IVec2 nd) {
        if (nd.x == 0 && nd.y == 0) return;
        const IVec2 last = turnQueue_.empty() ? dir_ : turnQueue_.back();
        if (nd == last || isOpposite(nd, last)) return;
        if (turnQueue_.size() < 3) turnQueue_.push_back(nd);
    }

    // padPressed needs an explicit pad slot; scan all four (the engine caps
    // gamepads at 4) so any connected controller can steer.
    static bool padPressedAny(const forge::Input& in, forge::GamepadButton b) {
        for (int p = 0; p < 4; ++p)
            if (in.padPressed(p, b)) return true;
        return false;
    }

    void queueTurns(const forge::Input& in) {
        if (in.pressed(forge::Key::W) || in.pressed(forge::Key::Up) ||
            padPressedAny(in, forge::GamepadButton::DpadUp))
            queueDir({0, 1});
        if (in.pressed(forge::Key::S) || in.pressed(forge::Key::Down) ||
            padPressedAny(in, forge::GamepadButton::DpadDown))
            queueDir({0, -1});
        if (in.pressed(forge::Key::A) || in.pressed(forge::Key::Left) ||
            padPressedAny(in, forge::GamepadButton::DpadLeft))
            queueDir({-1, 0});
        if (in.pressed(forge::Key::D) || in.pressed(forge::Key::Right) ||
            padPressedAny(in, forge::GamepadButton::DpadRight))
            queueDir({1, 0});

        // Analog stick: treat a decisive tilt as one turn per new direction
        // (edge-triggered so holding the stick doesn't spam the queue).
        IVec2 sd{0, 0};
        for (int p = 0; p < 4; ++p) {
            const float ax = in.padAxis(p, 0);
            const float aySdl = in.padAxis(p, 1);  // SDL: down is positive
            if (std::fabs(ax) > 0.55f || std::fabs(aySdl) > 0.55f) {
                sd = std::fabs(ax) > std::fabs(aySdl)
                         ? IVec2{ax > 0.0f ? 1 : -1, 0}
                         : IVec2{0, aySdl > 0.0f ? -1 : 1};
            }
        }
        if (!(sd == stickDir_)) {
            stickDir_ = sd;
            if (!(sd == IVec2{0, 0})) queueDir(sd);
        }
    }

    // ------------------------------------------------------------ simulation
    void applyTurn(bool bot) {
        if (bot) {
            const IVec2 nd = botDir();
            if (!(nd == dir_) && !isOpposite(nd, dir_)) dir_ = nd;
            return;
        }
        while (!turnQueue_.empty()) {
            const IVec2 nd = turnQueue_.front();
            turnQueue_.pop_front();
            if (nd == dir_ || isOpposite(nd, dir_)) continue;
            dir_ = nd;
            break;
        }
    }

    bool selfHit(IVec2 c, bool ignoreTail) const {
        const size_t n = body_.size();
        for (size_t i = 0; i < n; ++i)
            if (body_[i] == c && !(ignoreTail && i + 1 == n)) return true;
        return false;
    }

    bool blockedFor(IVec2 c, bool allowTail) const {
        return !inBounds(c) || selfHit(c, allowTail);
    }

    void stepOnce(bool bot) {
        applyTurn(bot);

        const IVec2 nh = head() + dir_;
        const bool growing = growPending_ > 0;
        if (!inBounds(nh) || selfHit(nh, !growing)) {
            die(nh);
            return;
        }

        from_ = body_;
        body_.insert(body_.begin(), nh);
        if (growing) --growPending_;
        else body_.pop_back();

        if (nh == food_) eat(false);
        else if (goldenActive_ && nh == golden_) eat(true);
    }

    void simulate(float dt, bool bot) {
        runTime_ += dt;
        stepTimer_ += dt;
        while (!death_ && stepTimer_ >= stepInterval()) {
            stepTimer_ -= stepInterval();
            stepOnce(bot);
        }
        interp_ = death_ ? 1.0f : forge::clamp01(stepTimer_ / stepInterval());
        tickGolden(dt);
    }

    void eat(bool gold) {
        headFlash_ = 0.14f;
        const IVec2 at = gold ? golden_ : food_;
        if (gold) {
            score_ += GOLD_SCORE;
            growPending_ += 2;
            goldenActive_ = false;
            addPopup(cellCenter(at), GOLD_SCORE, true);
            playSfx(sfx_.gold, 0.95f, panFor(cellCenter(at).x));
            cam.addTrauma(0.10f);
            fxGold().setPosition(cellCenter(at));
            fxGold().emit(30);
        } else {
            score_ += APPLE_SCORE;
            growPending_ += 1;
            ++applesEaten_;
            addPopup(cellCenter(at), APPLE_SCORE, false);
            playSfx(sfx_.eat, 0.9f, panFor(cellCenter(at).x),
                    rng_.range(0.97f, 1.06f));
            cam.addTrauma(0.06f);
            fxEat().setPosition(cellCenter(at));
            fxEat().emit(16);
            if (applesEaten_ % GOLD_EVERY == 0) spawnGolden();
        }
        if (score_ > highScore_) { highScore_ = score_; newHigh_ = true; }
        if (!spawnCell(food_)) {
            score_ += PERFECT_BONUS;  // board cleared!
            if (score_ > highScore_) { highScore_ = score_; newHigh_ = true; }
            endRun(true, head());
        }
    }

    void die(IVec2 hit) { endRun(false, hit); }

    void endRun(bool perfect, IVec2 hit) {
        death_ = true;
        deathTimer_ = 0.0f;
        perfect_ = perfect;
        interp_ = 1.0f;
        if (score_ > highScore_) { highScore_ = score_; newHigh_ = true; }
        if (perfect) {
            cam.addTrauma(0.35f);
            playSfx(sfx_.perfect, 0.95f);
            for (int i = 0; i < 4; ++i) {
                fxGold().setPosition({110.0f + (float)i * 140.0f, 300.0f});
                fxGold().emit(24);
            }
        } else {
            deathCell_ = clampCell(hit);
            cam.addTrauma(0.65f);
            playSfx(sfx_.die, 0.95f, 0.0f, rng_.range(0.97f, 1.03f));
            forge::ParticleSystem& fx = fxDie();
            for (size_t i = 0; i < body_.size(); i += 3) {
                fx.setPosition(cellCenter(body_[i]));
                fx.emit(3);
            }
            fx.setPosition(cellCenter(deathCell_));
            fx.emit(28);
        }
    }

    // ------------------------------------------------------------ bot
    // Greedy with a survival filter: chase the nearest prize, avoid walls and
    // (unless --sloppy) your own body. Good enough to demo, and it pins the
    // same code path the human queue feeds, so captures stay deterministic.
    IVec2 botDir() const {
        IVec2 target = food_;
        if (goldenActive_ &&
            manhattan(head(), golden_) < manhattan(head(), food_))
            target = golden_;
        const bool allowTail = growPending_ == 0;
        const IVec2 cands[4] = {{0, 1}, {-1, 0}, {0, -1}, {1, 0}};
        IVec2 best = dir_;
        float bestScore = -1e9f;
        for (const IVec2& c : cands) {
            if (isOpposite(c, dir_)) continue;
            const IVec2 nh = head() + c;
            const bool safe =
                sloppy_ ? inBounds(nh) : !blockedFor(nh, allowTail);
            float s = (safe ? 1000.0f : 0.0f) -
                      (float)manhattan(nh, target) +
                      (c == dir_ ? 0.5f : 0.0f);
            if (s > bestScore) { bestScore = s; best = c; }
        }
        return best;
    }

    // ------------------------------------------------------------ effects
    void updateEffects(float dt) {
        for (auto& fx : fx_) fx.update(dt);
        for (size_t i = 0; i < popups_.size();) {
            Popup& p = popups_[i];
            p.life -= dt;
            p.pos.y += 30.0f * dt;
            if (p.life <= 0.0f) popups_[i] = popups_.back(), popups_.pop_back();
            else ++i;
        }
        headFlash_ = std::max(0.0f, headFlash_ - dt);
    }

    void addPopup(Vec2 at, int value, bool gold) {
        if (popups_.size() >= 24) popups_.erase(popups_.begin());
        popups_.push_back({at + Vec2{rng_.range(-7.0f, 7.0f), 0.0f}, value,
                           gold, 0.8f});
    }

    // ------------------------------------------------------------ drawing
    static void roundRect(forge::SpriteBatch& b, Vec2 c, float w, float h,
                          float r, Color col, int seg = 10) {
        r = std::min(r, std::min(w, h) * 0.5f);
        const float x0 = c.x - w * 0.5f, y0 = c.y - h * 0.5f;
        b.drawRect({x0, y0 + r, w, h - 2.0f * r}, col);
        b.drawRect({x0 + r, y0, w - 2.0f * r, h}, col);
        b.drawCircle({x0 + r, y0 + r}, r, col, seg);
        b.drawCircle({x0 + w - r, y0 + r}, r, col, seg);
        b.drawCircle({x0 + r, y0 + h - r}, r, col, seg);
        b.drawCircle({x0 + w - r, y0 + h - r}, r, col, seg);
    }

    // Interpolated world position of body slot i: cells glide from the
    // previous step's cell into the current one by `interp_`.
    Vec2 segWorld(size_t i, float t) const {
        const IVec2 a = (i < from_.size()) ? from_[i] : body_[i];
        const Vec2 ca = cellCenter(a);
        const Vec2 cb = cellCenter(body_[i]);
        return {forge::lerp(ca.x, cb.x, t), forge::lerp(ca.y, cb.y, t)};
    }

    Color segColor(size_t i) const {
        const size_t n = body_.size();
        const float u = n > 1 ? (float)i / (float)(n - 1) : 0.0f;
        Color c = kSnakeHead.lerped(kSnakeTail, std::pow(u, 0.85f));
        const float wave = 0.94f + 0.10f * std::sin(uiTime_ * 3.5f - (float)i * 0.55f);
        c = shade(c, wave);
        if (death_) {
            const float k = forge::clamp01(deathTimer_ / 0.5f);
            c = c.lerped(perfect_ ? kGold : kDanger, 0.35f * k);
        }
        float flash = 0.0f;
        if (death_ && !perfect_)
            flash = std::max(0.0f, 1.0f - deathTimer_ * 6.0f);
        if (i == 0 && headFlash_ > 0.0f)
            flash = std::max(flash, forge::clamp01(headFlash_ / 0.14f) * 0.75f);
        return c.lerped(Color::white(), flash);
    }

    void drawField(forge::SpriteBatch& b) {
        for (int y = 0; y < GRID_H; ++y)
            for (int x = 0; x < GRID_W; ++x)
                b.drawRect({WALL + (float)x * CELL, WALL + (float)y * CELL, CELL, CELL},
                           ((x + y) & 1) ? kPanelAlt : kPanel);

        // Wall band + block pattern + inner rim.
        b.drawRect({0, 0, FIELD_W, WALL}, kWall);
        b.drawRect({0, PLAY_H - WALL, FIELD_W, WALL}, kWall);
        b.drawRect({0, WALL, WALL, PLAY_H - 2 * WALL}, kWall);
        b.drawRect({FIELD_W - WALL, WALL, WALL, PLAY_H - 2 * WALL}, kWall);
        for (int i = 0; i < GRID_W; ++i) {
            if (!(i & 1)) continue;
            const float x = WALL + (float)i * CELL;
            b.drawRect({x, PLAY_H - WALL, CELL, WALL}, kWallAlt);
            b.drawRect({x, 0, CELL, WALL}, kWallAlt);
        }
        for (int j = 0; j < GRID_H; ++j) {
            if (!(j & 1)) continue;
            const float y = WALL + (float)j * CELL;
            b.drawRect({0, y, WALL, CELL}, kWallAlt);
            b.drawRect({FIELD_W - WALL, y, WALL, CELL}, kWallAlt);
        }
        b.drawRect({WALL - 2, WALL - 2, (float)GRID_W * CELL + 4, 2}, kWallEdge);
        b.drawRect({WALL - 2, PLAY_H - WALL, (float)GRID_W * CELL + 4, 2}, kWallEdge);
        b.drawRect({WALL - 2, WALL - 2, 2, (float)GRID_H * CELL + 4}, kWallEdge);
        b.drawRect({FIELD_W - WALL, WALL - 2, 2, (float)GRID_H * CELL + 4}, kWallEdge);

        // HUD bar.
        b.drawRect({0, PLAY_H, FIELD_W, HUD_H}, kHudBar);
        b.drawRect({0, PLAY_H, FIELD_W, 1.5f}, kAccent);
    }

    void drawFood(forge::SpriteBatch& b) {
        const Vec2 sh{-2.2f, -2.8f};  // shared drop-shadow offset
        const Color shadow{0.0f, 0.0f, 0.0f, 0.22f};

        // Apple: red body, dark rim, stem + leaf, gentle pulse.
        const Vec2 ap = cellCenter(food_);
        const float pulse = 1.0f + 0.06f * std::sin(uiTime_ * 5.0f);
        const float r = CELL * 0.36f * pulse;
        b.drawCircle(ap + sh, r, shadow, 20);
        b.drawCircle(ap, r, kAppleDark, 20);
        b.drawCircle(ap + Vec2{0.0f, 0.4f}, r * 0.88f, kApple, 20);
        b.drawCircle(ap + Vec2{-r * 0.28f, r * 0.30f}, r * 0.22f,
                     withA(Color::white(), 0.85f), 8);
        b.drawLine(ap + Vec2{0.0f, r * 0.9f}, ap + Vec2{1.6f, r * 1.65f}, kStem, 2.0f);
        b.drawCircle(ap + Vec2{3.4f, r * 1.45f}, r * 0.30f, kLeaf, 8);

        // Golden bonus: spinning rays, blinking when about to expire.
        if (goldenActive_) {
            float fade = 1.0f;
            if (goldenTimer_ < 2.0f)
                fade = 0.45f + 0.55f * std::fabs(std::sin(uiTime_ * 9.0f));
            const Vec2 gp = cellCenter(golden_);
            const float pr = CELL * 0.33f * (1.0f + 0.08f * std::sin(uiTime_ * 6.0f));
            b.drawCircle(gp + sh, pr, shadow, 20);
            for (int k = 0; k < 4; ++k) {
                const float a = uiTime_ * 1.8f + (float)k * forge::TAU * 0.25f;
                const Vec2 d{std::cos(a), std::sin(a)};
                b.drawLine(gp + d * (pr * 1.05f), gp + d * (pr * 1.8f),
                           withA(kGold, 0.9f * fade), 2.5f);
            }
            b.drawCircle(gp, pr, withA(kGoldDeep, fade), 18);
            b.drawCircle(gp, pr * 0.82f, withA(kGold, fade), 18);
            b.drawCircle(gp + Vec2{-pr * 0.25f, pr * 0.28f}, pr * 0.22f,
                         withA(Color::white(), 0.9f * fade), 8);
        }
    }

    void drawSnake(forge::SpriteBatch& b) {
        const int n = (int)body_.size();
        if (n == 0) return;
        const float t = interp_;
        const Vec2 sh{-2.2f, -2.8f};  // drop shadow: lifts the snake off the board
        const Color shadow{0.0f, 0.0f, 0.0f, 0.22f};

        // Shadow pass (links + bodies), then the snake itself on top.
        for (int i = n - 1; i >= 1; --i)
            b.drawLine(segWorld((size_t)i, t) + sh, segWorld((size_t)(i - 1), t) + sh,
                       shadow, CELL * 0.60f);
        for (int i = n - 1; i >= 0; --i) {
            const float inset = (i == 0) ? 0.5f : 1.5f;
            const float size = CELL - inset * 2.0f;
            roundRect(b, segWorld((size_t)i, t) + sh, size, size, size * 0.36f, shadow);
        }

        // Links first so the segment bodies cap the joints.
        for (int i = n - 1; i >= 1; --i)
            b.drawLine(segWorld((size_t)i, t), segWorld((size_t)(i - 1), t),
                       segColor((size_t)i), CELL * 0.62f);

        for (int i = n - 1; i >= 0; --i) {
            const Vec2 p = segWorld((size_t)i, t);
            const float inset = (i == 0) ? 0.5f : 1.5f;
            const float size = CELL - inset * 2.0f;
            roundRect(b, p, size, size, size * 0.36f, segColor((size_t)i));
            // Slither glint: a soft highlight slides along the body.
            if (i > 0 && ((i + (int)(uiTime_ * 9.0f)) % 7) == 0)
                b.drawCircle(p + Vec2{-2.0f, 2.0f}, 2.2f,
                             withA(Color::white(), 0.10f), 8);
        }

        // Head eyes look where the snake is going.
        const Vec2 hp = segWorld(0, t);
        const Vec2 d{(float)dir_.x, (float)dir_.y};
        const Vec2 pd{-d.y, d.x};
        for (const float side : {-1.0f, 1.0f}) {
            const Vec2 eye = hp + d * 3.4f + pd * (side * 3.6f);
            b.drawCircle(eye, 2.9f, Color::white(), 12);
            b.drawCircle(eye + d * 1.2f, 1.5f, kPanel, 8);
        }
        // Tongue flick.
        const float flick = std::sin(uiTime_ * 2.1f);
        if (flick > 0.86f && !death_) {
            const Vec2 tip = hp + d * 11.5f;
            b.drawLine(hp + d * 8.0f, tip, kDanger, 1.6f);
            b.drawLine(tip, tip + pd * 2.2f, kDanger, 1.4f);
            b.drawLine(tip, tip - pd * 2.2f, kDanger, 1.4f);
        }
    }

    void drawPopups(forge::SpriteBatch& b) {
        for (const Popup& p : popups_) {
            char buf[24];
            if (p.gold) std::snprintf(buf, sizeof(buf), "GOLD +%d", p.value);
            else std::snprintf(buf, sizeof(buf), "+%d", p.value);
            const float a = forge::clamp01(p.life / 0.8f);
            drawTextCentered(b, buf, p.pos + Vec2{0.0f, 12.0f}, 0.55f,
                             withA(p.gold ? kGold : shade(kSnakeHead, 1.2f), a));
        }
    }

    void drawHud(forge::SpriteBatch& b) {
        if (screen_ == Screen::Title) return;  // demo stats stay hidden on the menu
        char buf[64];
        const Color dim = withA(Color::white(), 0.45f);
        const float y = PLAY_H + 12.0f;

        drawText(b, "SCORE", {26.0f, y}, 0.58f, dim);
        std::snprintf(buf, sizeof(buf), "%06d", score_);
        drawText(b, buf, {26.0f + widthOf("SCORE", 0.58f) + 14.0f, y - 2.0f},
                 0.72f, Color::white());

        std::snprintf(buf, sizeof(buf), "HIGH %06d", highScore_);
        drawTextCentered(b, buf, {FIELD_W * 0.5f, PLAY_H + 20.0f}, 0.58f,
                         newHigh_ ? kGold : dim);

        std::snprintf(buf, sizeof(buf), "LEN %02d", (int)body_.size());
        drawText(b, buf, {FIELD_W - 26.0f - widthOf(buf, 0.58f), y}, 0.58f, dim);

        if (goldenActive_) {  // bonus countdown pip
            std::snprintf(buf, sizeof(buf), "GOLD %.1f", std::max(0.0f, goldenTimer_));
            drawTextCentered(b, buf, {FIELD_W * 0.5f, 34.0f}, 0.5f, withA(kGold, 0.7f));
        }

        if (screen_ == Screen::Playing && !death_)
            drawTextCentered(b, "WASD / ARROWS MOVE   ESC PAUSE   R RESTART   Q QUIT",
                             {FIELD_W * 0.5f, 10.0f}, 0.48f, withA(Color::white(), 0.20f));
    }

    void drawOverlays(forge::SpriteBatch& b) {
        auto dimField = [&](float a) {
            b.drawRect({0, 0, FIELD_W, PLAY_H}, withA(Color::black(), a));
        };
        if (death_ && !perfect_) {  // impact flash that fades into the overlay
            dimField(0.30f * forge::clamp01(1.0f - deathTimer_ * 3.5f));
        }
        if (paused_) {
            dimField(0.62f);
            drawBig(b, "PAUSED", 300.0f, 0.85f, Color::white());
            drawTextCentered(b, "ESC RESUME   R RESTART   Q QUIT",
                             {FIELD_W * 0.5f, 250.0f}, 0.62f,
                             withA(Color::white(), 0.7f));
            return;
        }
        switch (screen_) {
            case Screen::Title: {
                dimField(0.70f);
                char buf[64];
                drawBig(b, "SNAKE", 348.0f, 1.0f, Color::white());
                drawTextCentered(b, "A FORGE ENGINE DEMO", {FIELD_W * 0.5f, 300.0f},
                                 0.6f, withA(kSnakeHead, 0.9f));
                std::snprintf(buf, sizeof(buf), "HIGH SCORE  %06d", highScore_);
                drawTextCentered(b, buf, {FIELD_W * 0.5f, 264.0f}, 0.62f, kGold);
                const float blink = 0.55f + 0.45f * std::sin(uiTime_ * 5.0f);
                drawTextCentered(b, "PRESS SPACE TO START",
                                 {FIELD_W * 0.5f, 200.0f}, 0.72f,
                                 withA(Color::white(), blink));
                const Color hint = withA(Color::white(), 0.5f);
                drawTextCentered(b, "MOVE   WASD  ARROWS  GAMEPAD",
                                 {FIELD_W * 0.5f, 150.0f}, 0.55f, hint);
                drawTextCentered(b, "APPLE +10   GOLD +50   DO NOT BITE YOURSELF",
                                 {FIELD_W * 0.5f, 128.0f}, 0.55f, hint);
                drawTextCentered(b, "ESC PAUSE      R RESTART      Q QUIT",
                                 {FIELD_W * 0.5f, 106.0f}, 0.55f, hint);
                drawTextCentered(b, "BOT DEMO PLAYING BEHIND THIS MENU",
                                 {FIELD_W * 0.5f, 78.0f}, 0.48f,
                                 withA(Color::white(), 0.28f));
                break;
            }
            case Screen::Playing:
                break;
            case Screen::GameOver: {
                dimField(0.72f);
                char buf[64];
                if (perfect_) {
                    drawBig(b, "PERFECT!", 348.0f, 0.95f, kGold);
                    drawTextCentered(b, "BOARD CLEARED", {FIELD_W * 0.5f, 300.0f},
                                     0.62f, withA(kGold, 0.9f));
                } else {
                    drawBig(b, "GAME OVER", 348.0f, 0.95f, kDanger);
                }
                std::snprintf(buf, sizeof(buf), "SCORE  %06d", score_);
                drawTextCentered(b, buf, {FIELD_W * 0.5f, 272.0f}, 0.72f, Color::white());
                std::snprintf(buf, sizeof(buf), "HIGH   %06d", highScore_);
                drawTextCentered(b, buf, {FIELD_W * 0.5f, 246.0f}, 0.62f,
                                 newHigh_ ? kGold : withA(Color::white(), 0.7f));
                std::snprintf(buf, sizeof(buf), "APPLES %d   LENGTH %d   %dS",
                              applesEaten_, (int)body_.size(), (int)runTime_);
                drawTextCentered(b, buf, {FIELD_W * 0.5f, 220.0f}, 0.55f,
                                 withA(Color::white(), 0.6f));
                if (newHigh_) {
                    const float blink = 0.55f + 0.45f * std::sin(uiTime_ * 5.0f);
                    drawTextCentered(b, "NEW HIGH SCORE!", {FIELD_W * 0.5f, 190.0f},
                                     0.62f, withA(kGold, blink));
                }
                const float blink = 0.55f + 0.45f * std::sin(uiTime_ * 5.0f);
                drawTextCentered(b, "PRESS SPACE TO PLAY AGAIN",
                                 {FIELD_W * 0.5f, 148.0f}, 0.68f,
                                 withA(Color::white(), blink));
                break;
            }
        }
    }

    void drawText(forge::SpriteBatch& b, const std::string& s, Vec2 pos,
                  float scale, Color c) {
        if (font_.valid()) font_.draw(b, s, pos, scale, c);
    }
    void drawTextCentered(forge::SpriteBatch& b, const std::string& s, Vec2 center,
                          float scale, Color c) {
        if (font_.valid()) font_.drawCentered(b, s, center, scale, c);
    }
    float widthOf(const std::string& s, float scale) {
        return font_.valid() ? font_.widthOf(s, scale) : 0.0f;
    }
    void drawBig(forge::SpriteBatch& b, const std::string& s, float cy,
                 float relScale, Color c) {
        if (bigFont_ && bigFont_->valid())
            bigFont_->drawCentered(b, s, {FIELD_W * 0.5f, cy}, bigScale_ * relScale, c);
    }

    // ------------------------------------------------------------ state
    struct Sfx {
        forge::SoundHandle eat, gold, spawn, expire, die, over, start, pause, perfect;
    };

    forge::Engine* engine_ = nullptr;
    forge::Camera2D cam;
    forge::Font font_;
    forge::Font fontBig_;
    const forge::Font* bigFont_ = nullptr;
    float bigScale_ = 1.0f;
    bool haveBig_ = false;

    forge::ParticleSystem fx_[3];
    forge::ParticleSystem& fxEat() { return fx_[0]; }
    forge::ParticleSystem& fxGold() { return fx_[1]; }
    forge::ParticleSystem& fxDie() { return fx_[2]; }
    Sfx sfx_;

    Screen screen_ = Screen::Title;
    bool paused_ = false;
    bool autoplay_ = false;
    bool sloppy_ = false;
    float uiTime_ = 0.0f;
    float titleTimer_ = 0.0f;
    float overTimer_ = 0.0f;

    // Snake state: body_[0] is the head. `from_` is the pre-step snapshot
    // (same slots) that segment positions interpolate out of each frame.
    std::vector<IVec2> body_;
    std::vector<IVec2> from_;
    IVec2 dir_{1, 0};
    std::deque<IVec2> turnQueue_;
    IVec2 stickDir_{0, 0};
    int growPending_ = 0;

    IVec2 food_{GRID_W / 2 - 3, GRID_H / 2};
    bool goldenActive_ = false;
    IVec2 golden_{0, 0};
    float goldenTimer_ = 0.0f;

    float stepTimer_ = 0.0f;
    float interp_ = 0.0f;
    bool death_ = false;
    bool perfect_ = false;
    float deathTimer_ = 0.0f;
    IVec2 deathCell_{0, 0};
    float headFlash_ = 0.0f;
    float runTime_ = 0.0f;

    int score_ = 0, highScore_ = 0;
    int applesEaten_ = 0;
    bool newHigh_ = false;

    std::vector<Popup> popups_;
    forge::Random rng_{20261019};
};

// ------------------------------------------------------------------- util
struct Args {
    std::string shot;
    int frames = 0;
    uint64_t seed = 20261019;
    int pauseAt = 0;                 // --pause-at: ESC tap frame (shot mode)
    int width = 1024, height = 768;  // --shot capture size
    bool title = false;
    bool noAi = false;
    bool sloppy = false;
    bool demo = false;
    bool help = false;
};

Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](void) -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (arg == "--shot") { if (const char* v = next()) a.shot = v; }
        else if (arg == "--frames") { if (const char* v = next()) a.frames = std::atoi(v); }
        else if (arg == "--seed") { if (const char* v = next()) a.seed = std::strtoull(v, nullptr, 10); }
        else if (arg == "--pause-at") { if (const char* v = next()) a.pauseAt = std::atoi(v); }
        else if (arg == "--size") {
            if (const char* v = next()) std::sscanf(v, "%dx%d", &a.width, &a.height);
        }
        else if (arg == "--title") a.title = true;
        else if (arg == "--no-ai") a.noAi = true;
        else if (arg == "--sloppy") a.sloppy = true;
        else if (arg == "--demo") a.demo = true;
        else if (arg == "-h" || arg == "--help") a.help = true;
    }
    if (a.width < 64) a.width = 64;
    if (a.height < 64) a.height = 64;
    return a;
}

void printUsage() {
    std::printf(
        "08_snake — classic Snake on Forge\n"
        "  WASD/arrows/gamepad move · SPACE/click/A start · ESC pause\n"
        "  R restart · Q quit\n"
        "Options:\n"
        "  --shot FILE   render N fixed steps with the autopilot, save PNG, exit\n"
        "  --frames N    frames to simulate/hold before exit (shot/CI)\n"
        "  --size WxH    capture size for --shot (default 1024x768, offscreen)\n"
        "  --title       stay on the title screen (shot mode)\n"
        "  --demo        watch the autopilot play (interactive)\n"
        "  --no-ai       disable the autopilot\n"
        "  --sloppy      autopilot plays badly (capture death screens)\n"
        "  --pause-at N  tap ESC on frame N (shot mode: pause overlay)\n"
        "  --seed N      RNG seed\n");
}

bool saveScreenshot(const std::string& path, forge::IVec2 vp) {
    std::vector<uint8_t> px((size_t)vp.x * (size_t)vp.y * 4);
    forge::gl::glReadPixels(0, 0, vp.x, vp.y, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    std::vector<uint8_t> flipped(px.size());  // GL is bottom-up, PNG is top-down
    const size_t row = (size_t)vp.x * 4;
    for (int y = 0; y < vp.y; ++y)
        std::memcpy(&flipped[(size_t)y * row],
                    &px[(size_t)(vp.y - 1 - y) * row], row);
    return stbi_write_png(path.c_str(), vp.x, vp.y, 4, flipped.data(), (int)row) != 0;
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parseArgs(argc, argv);
    if (args.help) { printUsage(); return 0; }
    const bool ci = std::getenv("CI") != nullptr;

    forge::EngineConfig cfg;
    cfg.window.title = "Forge 08_snake — classic Snake";
    cfg.window.width = 1024;
    cfg.window.height = 768;              // 4:3: the playfield fills it
    cfg.window.vsync = args.shot.empty();
    cfg.fixedStep = 1.0f / 60.0f;
    forge::Engine engine(cfg);
    if (!engine.valid()) return 1;

    Snake game;
    game.setSeed(args.seed);
    game.setSloppyBot(args.sloppy);
    game.init(engine);

    if (!args.shot.empty()) {
        // Deterministic capture: drive the simulation with a fixed dt (no wall
        // clock), render once into a 4:3 offscreen target, read it back.
        const int frames = args.frames > 0 ? args.frames : 1200;
        game.setAutoplay(!args.noAi && !args.title);
        if (!args.title) game.startGame();
        for (int i = 0; i < frames; ++i) {
            if (args.pauseAt > 0) {
                // Simulate a one-frame ESC tap through the real Input object,
                // exercising the keyboard -> pause path (and freezing the sim).
                // beginFrame() first: it folds the previous frame's state into
                // prev and clears latches, so the down-edge lands in this frame.
                engine.input().beginFrame();
                engine.input().setKey(forge::Key::Escape, i == args.pauseAt);
            }
            game.update(1.0f / 60.0f);
        }

        forge::Framebuffer fbo;
        const bool offscreen = fbo.create(args.width, args.height);
        game.renderTo(offscreen ? &fbo : nullptr);
        const forge::IVec2 shotSize = offscreen ? forge::IVec2{args.width, args.height}
                                                : engine.viewport();
        if (offscreen) fbo.bind();  // read back the target we just rendered into
        const bool ok = saveScreenshot(args.shot, shotSize);
        if (offscreen)
            forge::Framebuffer::unbind(engine.viewport().x, engine.viewport().y);
        const bool sane = game.healthy();
        std::printf("shot: %s (%dx%d, %d frames) score=%06d apples=%d len=%d "
                    "perfect=%d draws=%d sane=%d %s\n",
                    args.shot.c_str(), shotSize.x, shotSize.y, frames, game.score(),
                    game.apples(), game.length(), game.perfect() ? 1 : 0,
                    engine.drawCalls(), sane ? 1 : 0, ok ? "OK" : "FAILED");
        return ok && sane ? 0 : 1;
    }

    game.setAutoplay((args.demo || ci) ? !args.noAi : false);
    const int frameLimit = args.frames > 0 ? args.frames : (ci ? 600 : 0);
    int frames = 0;
    engine.runFixed(
        [&](float dt) { game.update(dt); },
        [&] {
            game.render();
            if (frameLimit > 0 && ++frames >= frameLimit) engine.quit();
        });
    std::printf("snake: score=%06d apples=%d length=%d sane=%d\n",
                game.score(), game.apples(), game.length(), game.healthy() ? 1 : 0);
    return game.healthy() ? 0 : 1;
}
