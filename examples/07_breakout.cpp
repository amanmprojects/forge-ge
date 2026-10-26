// 07_breakout — a complete Atari-style Breakout, built on Forge.
//
//   move    A/D · arrows · mouse · gamepad stick/dpad
//   launch  SPACE · click · gamepad A       confirm on menus
//   pause   ESC          restart  R          quit  Q
//
// Everything is procedural: bricks/HUD are batched white-quad shapes and
// TTF text, SFX are synthesized WAVs at startup (no asset files), break
// effects are ParticleSystem bursts, impacts use Camera2D trauma shake.
//
// Headless capture (docs/CI): runs N fixed steps with a scripted autopilot,
// writes the final frame to a PNG and exits.
//   ./07_breakout --shot out.png --frames 900 [--level 3] [--title] [--no-ai]
//   ./07_breakout --record clip.mp4 --frames 1200   # autopilot video (ffmpeg)
// CI=1 ./07_breakout [--frames 240]      # autopilot smoke run, then exit 0
#include <forge/forge.hpp>

#include "clip.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
using forge::Rect;
using forge::Vec2;

// ------------------------------------------------------------------ layout
// Fixed 640x480 logical playfield (4:3), letterboxed into any window by a
// fitted Camera2D. All coordinates below are playfield units, Y-up.
constexpr float FIELD_W = 640.0f, FIELD_H = 480.0f;
constexpr float WALL = 16.0f;                    // side wall thickness
constexpr float HUD_H = 34.0f;                   // top bar (doubles as ceiling)
constexpr float PLAY_TOP = FIELD_H - HUD_H;
constexpr float PADDLE_W = 88.0f, PADDLE_H = 14.0f, PADDLE_Y = 64.0f;
constexpr float PADDLE_SPEED = 540.0f;
constexpr float PADDLE_MIN_X = WALL + PADDLE_W * 0.5f;
constexpr float PADDLE_MAX_X = FIELD_W - WALL - PADDLE_W * 0.5f;
constexpr float BALL_R = 7.0f;
constexpr float BALL_BASE_SPEED = 305.0f;        // px/s at level 1, no bricks
constexpr float BALL_MAX_SPEED = 620.0f;         // cap: keep bounce readable
constexpr float SERVE_TILT_MAX = 0.50f;          // rad, launch spread
constexpr float BOUNCE_ANGLE_MAX = 1.05f;        // rad (~60 deg) at paddle edge
constexpr float PADDLE_ENGLISH = 0.12f;          // sweep -> sidespin (--english scales it)
constexpr float MIN_VY_FRAC = 0.38f;             // never let the ball go flat
constexpr float MIN_VX_FRAC = 0.05f;             // ...nor perfectly vertical
constexpr int LIVES_START = 3;

constexpr int COLS = 8, ROWS = 6;
constexpr float BRICK_H = 22.0f, BRICK_GAP = 8.0f, BRICK_MARGIN = 10.0f;
constexpr float BRICK_W =
    (FIELD_W - 2 * WALL - 2 * BRICK_MARGIN - (COLS - 1) * BRICK_GAP) / COLS;
constexpr float BRICK_TOP = PLAY_TOP - 8.0f;     // top edge of brick row 0

// ------------------------------------------------------------------ palette
const Color kRowColors[ROWS] = {
    Color::fromHex(0xEF476FFF), Color::fromHex(0xF3722CFF),
    Color::fromHex(0xFFD166FF), Color::fromHex(0x06D6A0FF),
    Color::fromHex(0x4CC9F0FF), Color::fromHex(0x7B61FFFF),
};
const int kRowPoints[ROWS] = {7, 7, 4, 4, 1, 1};  // classic: top rows pay more

const Color kPanel = Color::fromHex(0x0C0C14FF);
const Color kWall = Color::fromHex(0x171724FF);
const Color kAccent = Color::fromHex(0x30305AFF);
const Color kHudBar = Color::fromHex(0x111119FF);
const Color kDanger = Color::fromHex(0x5A1F35FF);
const Color kPaddle = Color::fromHex(0xE9E9F2FF);
const Color kPaddleAccent = Color::fromHex(0x4CC9F0FF);
const Color kSteel = Color::fromHex(0x9AA5B1FF);
const Color kGold = Color::fromHex(0xFFD166FF);

static Color shade(Color c, float f) {
    return {std::min(1.0f, c.r * f), std::min(1.0f, c.g * f),
            std::min(1.0f, c.b * f), c.a};
}
static Color withA(Color c, float a) { return {c.r, c.g, c.b, a}; }

// ------------------------------------------------------------------ levels
struct LevelDef {
    const char* rows[ROWS];  // '#' brick, '%' 2-hit armored brick, '.' empty
};
const LevelDef kLevels[] = {
    {{"########", "########", "########", "########", "########", "########"}},
    {{"#.#.#.#.", ".#.#.#.#", "#.#.#.#.", ".#.#.#.#", "#.#.#.#.", ".#.#.#.#"}},
    {{"...##...", "..####..", ".######.", "########", "........", "........"}},
    {{"%%%%%%%%", "#......#", "#.####.#", "#.####.#", "#......#", "%%%%%%%%"}},
    {{"..#..#..", "#.####.#", "########", "##.##.##", "..####..", ".#....#."}},
    {{"##....##", "##....##", "%%....%%", "##....##", "##....##", "##....##"}},
};
constexpr int kLevelCount = (int)(sizeof(kLevels) / sizeof(kLevels[0]));

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
struct Brick {
    Rect rect;
    Color color = Color::white();
    int points = 0;
    int hp = 0, maxHp = 1;
    float flash = 0.0f;
    int touchStamp = 0;  // set when hit this substep (no double-damage)
};

struct Popup {  // floating "+7" score text
    Vec2 pos;
    int value = 0;
    float life = 0.0f;
    Color color = Color::white();
};

class Breakout {
public:
    enum class State { Title, Serve, Play, LifeLost, LevelClear, GameOver };

    void init(forge::Engine& engine) {
        engine_ = &engine;
        fitCamera(engine.viewport());

        haveBig_ = loadFont(fontBig_, 54.0f, "FORGE_FONT_TITLE");
        loadFont(font_, 24.0f, "FORGE_FONT");
        bigFont_ = haveBig_ ? &fontBig_ : &font_;
        bigScale_ = haveBig_ ? 1.45f : 3.2f;

        initAudio();
        initParticles();

        // Title backdrop: a full wall of bricks already sitting behind the menu.
        loadPattern(0);
        enterServe();
        state_ = State::Title;
        stateTimer_ = 0.0f;
    }

    void setAutoplay(bool on) { autoplay_ = on; }
    void setSloppyBot(bool on) { sloppyBot_ = on; }
    void setSeed(uint64_t s) { rng_.seed(s); }
    void setEnglish(float f) { english_ = f; }

    int score() const { return score_; }
    int level() const { return level_; }
    int lives() const { return lives_; }
    int bricksLeft() const { return bricksLeft_; }

    // Invariant check for CI: a healthy frame never has NaN or an out-of-field
    // ball/paddle. --shot fails (exit 1) if this trips.
    bool healthy() const {
        if (std::isnan(ballPos_.x) || std::isnan(ballPos_.y) ||
            std::isnan(ballVel_.x) || std::isnan(ballVel_.y))
            return false;
        if (paddleX_ < PADDLE_MIN_X - 0.5f || paddleX_ > PADDLE_MAX_X + 0.5f)
            return false;
        if (state_ == State::Play) {
            float speed = ballVel_.length();
            if (speed < 10.0f || speed > BALL_MAX_SPEED + 40.0f) return false;
            if (ballPos_.x < WALL - 1.0f || ballPos_.x > FIELD_W - WALL + 1.0f)
                return false;
            if (ballPos_.y < -1.0f || ballPos_.y > PLAY_TOP + 1.0f) return false;
        }
        return true;
    }

    void startRun(int firstLevel = 1) {
        score_ = 0;
        lives_ = LIVES_START;
        newHigh_ = false;
        popups_.clear();
        startLevel(std::max(1, firstLevel));
    }

    // ------------------------------------------------------------ update
    void update(float dt) {
        uiTime_ += dt;  // keeps menus blinking even while paused
        auto& in = engine_->input();
        fitCamera(engine_->viewport());

        if (in.pressed(forge::Key::Q)) { engine_->quit(); return; }
        const bool playing = state_ == State::Play || state_ == State::Serve ||
                             state_ == State::LifeLost || state_ == State::LevelClear;
        if (in.pressed(forge::Key::Escape) && (playing || paused_)) paused_ = !paused_;
        if (paused_) return;
        if (in.pressed(forge::Key::R) && state_ != State::Title) { startRun(1); return; }

        stateTimer_ += dt;
        const bool confirm = !autoplay_ &&
            (in.pressed(forge::Key::Space) || in.pressed(forge::Key::Enter) ||
             in.mousePressed(forge::MouseButton::Left) ||
             in.padPressed(0, forge::GamepadButton::A));

        updatePaddle(dt, in);
        updatePopups(dt);

        switch (state_) {
            case State::Title:
                if (confirm || (autoplay_ && stateTimer_ > 0.5f)) startRun(1);
                break;
            case State::Serve:
                if (confirm || (autoplay_ && stateTimer_ > 0.4f)) launchBall();
                else ballPos_ = servePos();
                break;
            case State::Play: {
                stepBall(dt);
                trailTimer_ += dt;
                if (trailTimer_ >= 1.0f / 90.0f) {  // ball trail samples
                    trailTimer_ = 0.0f;
                    pushTrail(ballPos_);
                }
                if (state_ == State::Play && bricksLeft_ == 0) enterLevelClear();
                break;
            }
            case State::LifeLost:
                if (stateTimer_ > 1.1f) {
                    if (lives_ > 0) enterServe();
                    else enterGameOver();
                }
                break;
            case State::LevelClear:
                if (stateTimer_ > 1.6f) startLevel(level_ + 1);
                break;
            case State::GameOver:
                if (confirm || (autoplay_ && stateTimer_ > 1.2f)) startRun(1);
                break;
        }

        for (auto& fx : fx_) fx.update(dt);
        cam.update(dt);
        for (auto& b : bricks_) b.flash = std::max(0.0f, b.flash - dt);
        paddleFlash_ = std::max(0.0f, paddleFlash_ - dt);
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
        forge::gl::glClearColor(0.027f, 0.027f, 0.043f, 1.0f);
        forge::gl::glClear(GL_COLOR_BUFFER_BIT);
        fitCamera(vp);
        auto& b = engine_->batch();
        b.begin(cam.viewProj(vp.x, vp.y));

        drawField(b);
        drawBricks(b);
        for (const auto& fx : fx_) fx.draw(b);
        drawPaddle(b);
        drawBall(b);
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
        sfx_.wall = add({{200, 165, 0.045f, 0.30f, 2, 0.0f}});
        sfx_.paddle = add({{330, 235, 0.060f, 0.35f, 1, 0.0f}});
        sfx_.launch = add({{260, 780, 0.100f, 0.32f, 2, 0.0f}});
        sfx_.armor = add({{190, 150, 0.060f, 0.30f, 1, 0.55f}});
        for (int r = 0; r < ROWS; ++r) {
            // High rows pop higher — you can hear your way up the wall.
            float f = 470.0f + (ROWS - 1 - r) * 95.0f;
            sfx_.brick[r] = add({{f, f * 0.55f, 0.075f, 0.34f, 1, 0.25f}});
        }
        sfx_.lose = add({{440, 120, 0.45f, 0.40f, 1, 0.12f},
                         {110, 78, 0.25f, 0.35f, 2, 0.0f}});
        sfx_.clear = add({{523, 523, 0.09f, 0.32f, 2, 0.0f},
                          {659, 659, 0.09f, 0.32f, 2, 0.0f},
                          {784, 784, 0.18f, 0.34f, 2, 0.0f}});
        sfx_.over = add({{392, 392, 0.16f, 0.34f, 1, 0.0f},
                         {311, 311, 0.16f, 0.34f, 1, 0.0f},
                         {247, 247, 0.16f, 0.34f, 1, 0.0f},
                         {165, 110, 0.45f, 0.36f, 1, 0.10f}});
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
        for (int r = 0; r < ROWS; ++r)
            fx_[r].configure(burstCfg(kRowColors[r], 256, 0x1000u + r, 5.5f, 0.55f, 240.0f));
        fxWall().configure(burstCfg(Color::white(), 192, 0x2000u, 3.5f, 0.22f, 150.0f));
        fxLose().configure(burstCfg(Color::fromHex(0xFF6B6BFF), 320, 0x3000u, 8.0f, 0.80f, 260.0f));
        fxClear().configure(burstCfg(kGold, 384, 0x4000u, 7.0f, 0.95f, 340.0f));
    }

    // ------------------------------------------------------------ audio
    float panFor(float x) const { return forge::clamp((x / FIELD_W - 0.5f) * 1.4f, -1.0f, 1.0f); }
    void playSfx(const forge::SoundHandle& h, float vol, float pan = 0.0f, float pitch = 1.0f) {
        if (h.valid()) engine_->audio().play(h, vol, pan, pitch);
    }
    forge::ParticleSystem& fxWall() { return fx_[ROWS]; }
    forge::ParticleSystem& fxLose() { return fx_[ROWS + 1]; }
    forge::ParticleSystem& fxClear() { return fx_[ROWS + 2]; }
    static constexpr int FX_COUNT = ROWS + 3;

    // ------------------------------------------------------------ geometry
    static Rect brickRect(int col, int row) {
        return {WALL + BRICK_MARGIN + col * (BRICK_W + BRICK_GAP),
                BRICK_TOP - BRICK_H - row * (BRICK_H + BRICK_GAP),
                BRICK_W, BRICK_H};
    }
    Rect paddleRect() const {
        return {paddleX_ - PADDLE_W * 0.5f, PADDLE_Y, PADDLE_W, PADDLE_H};
    }
    Vec2 servePos() const { return {paddleX_, PADDLE_Y + PADDLE_H + BALL_R + 1.0f}; }

    void fitCamera(forge::IVec2 vp) {
        cam.position = {FIELD_W * 0.5f, FIELD_H * 0.5f};
        cam.zoom = std::min((float)vp.x / FIELD_W, (float)vp.y / FIELD_H);
    }

    // ------------------------------------------------------------ level flow
    void loadPattern(int idx) {
        bricks_.assign((size_t)(COLS * ROWS), Brick{});
        bricksLeft_ = 0;
        for (int r = 0; r < ROWS; ++r) {
            const char* row = kLevels[idx].rows[r];
            for (int c = 0; c < COLS; ++c) {
                char ch = row[c];
                if (ch == '.' || ch == '\0') continue;
                Brick& b = bricks_[(size_t)r * COLS + c];
                b.rect = brickRect(c, r);
                b.color = kRowColors[r];
                b.maxHp = b.hp = (ch == '%') ? 2 : 1;
                b.points = kRowPoints[r] * (b.maxHp == 2 ? 3 : 1);
                ++bricksLeft_;
            }
        }
    }

    void startLevel(int level) {
        level_ = std::max(1, level);
        loadPattern((level_ - 1) % kLevelCount);
        brokenInLevel_ = 0;
        enterServe();
    }

    void enterServe() {
        state_ = State::Serve;
        stateTimer_ = 0.0f;
        ballAttached_ = true;
        ballVel_ = {0.0f, 0.0f};
        ballPos_ = servePos();
        trailCount_ = 0;
        trailHead_ = 0;
    }

    float currentSpeed() const {
        return std::min(BALL_BASE_SPEED + (float)(level_ - 1) * 18.0f +
                            (float)brokenInLevel_ * 4.0f,
                        BALL_MAX_SPEED);
    }

    void launchBall() {
        ballAttached_ = false;
        float tilt = rng_.range(0.13f, SERVE_TILT_MAX) * (rng_.chance(0.5f) ? 1.0f : -1.0f);
        ballVel_ = {std::sin(tilt), std::cos(tilt)};
        ballVel_ = ballVel_ * currentSpeed();
        state_ = State::Play;
        stateTimer_ = 0.0f;
        playSfx(sfx_.launch, 0.8f);
    }

    void enterLevelClear() {
        const int bonus = 50 + level_ * 25;
        score_ += bonus;
        if (score_ > highScore_) { highScore_ = score_; newHigh_ = true; }
        clearBonus_ = bonus;
        state_ = State::LevelClear;
        stateTimer_ = 0.0f;
        cam.addTrauma(0.25f);
        playSfx(sfx_.clear, 0.9f);
        for (int i = 0; i < 3; ++i) {  // confetti across the wall
            fxClear().setPosition({110.0f + i * 210.0f, 300.0f});
            fxClear().emit(18);
        }
    }

    void enterGameOver() {
        state_ = State::GameOver;
        stateTimer_ = 0.0f;
        if (score_ > highScore_) { highScore_ = score_; newHigh_ = true; }
        cam.addTrauma(0.55f);
        playSfx(sfx_.over, 0.9f);
    }

    void onBallLost() {
        --lives_;
        state_ = State::LifeLost;
        stateTimer_ = 0.0f;
        trailCount_ = 0;
        cam.addTrauma(0.5f);
        playSfx(sfx_.lose, 0.9f, panFor(ballPos_.x));
        fxLose().setPosition({ballPos_.x, 16.0f});
        fxLose().emit(46);
    }

    // ------------------------------------------------------------ gameplay
    void updatePaddle(float dt, const forge::Input& in) {
        float targetVel = 0.0f;
        if (autoplay_) {
            // Bot: track the ball but bat it toward whichever side still has
            // bricks (offset from the ball so the return angle varies).
            float offset = 0.0f;
            if (!ballAttached_) {
                bool left = false, right = false;
                for (const Brick& br : bricks_) {
                    if (br.hp <= 0) continue;
                    (br.rect.center().x < ballPos_.x ? left : right) = true;
                    if (left && right) break;
                }
                if (left != right) offset = left ? 20.0f : -20.0f;
                else offset = std::sin(uiTime_ * 0.7f) > 0.0f ? 20.0f : -20.0f;
                if (sloppyBot_) offset += 78.0f * std::sin(uiTime_ * 3.1f);
            }
            float target = ballAttached_ ? FIELD_W * 0.5f : ballPos_.x + offset;
            targetVel = forge::clamp((target - paddleX_) * 16.0f,
                                     -PADDLE_SPEED, PADDLE_SPEED);
        } else {
            forge::IVec2 vp = engine_->viewport();
            Vec2 axis = in.moveAxis(-1);  // WASD/arrows + all gamepads
            if (std::fabs(axis.x) > 0.25f) {
                targetVel = forge::clamp(axis.x, -1.0f, 1.0f) * PADDLE_SPEED;
                mouseActive_ = false;
            } else {
                if (in.mouseDelta().lengthSq() > 1.0f) mouseActive_ = true;
                if (mouseActive_) {
                    float mx = cam.screenToWorld(in.mousePos(), vp.x, vp.y).x;
                    targetVel = forge::clamp((mx - paddleX_) * 18.0f, -820.0f, 820.0f);
                }
            }
        }
        paddleVel_ = forge::damp(paddleVel_, targetVel, 26.0f, dt);
        float prev = paddleX_;
        paddleX_ = forge::clamp(paddleX_ + paddleVel_ * dt, PADDLE_MIN_X, PADDLE_MAX_X);
        if (paddleX_ == PADDLE_MIN_X || paddleX_ == PADDLE_MAX_X) paddleVel_ = 0.0f;
        paddleVelActual_ = (paddleX_ - prev) / std::max(dt, 1e-5f);
    }

    // Keep the ball out of two dead ends: a flat trajectory (endless horizontal
    // rally) and a perfectly vertical one (endless rally inside a brick gap).
    void sanitizeDirection() {
        float speed = ballVel_.length();
        if (speed < 1e-4f) return;
        Vec2 d = ballVel_ / speed;
        const float sy = d.y < 0.0f ? -1.0f : 1.0f;
        const float sx = d.x < 0.0f ? -1.0f : 1.0f;
        if (std::fabs(d.y) < MIN_VY_FRAC) {
            d = {sx * std::sqrt(1.0f - MIN_VY_FRAC * MIN_VY_FRAC), sy * MIN_VY_FRAC};
        } else if (std::fabs(d.x) < MIN_VX_FRAC) {
            d = {sx * MIN_VX_FRAC, sy * std::sqrt(1.0f - MIN_VX_FRAC * MIN_VX_FRAC)};
        }
        ballVel_ = d * speed;
    }

    void setSpeed(float s) {
        if (ballVel_.lengthSq() > 1.0f) ballVel_ = ballVel_.normalized() * s;
    }

    void stepBall(float dt) {
        float dist = ballVel_.length() * dt;
        int steps = std::max(1, std::min((int)std::ceil(dist / (BALL_R * 0.5f)), 32));
        float h = dt / (float)steps;
        for (int i = 0; i < steps && state_ == State::Play; ++i) {
            ++stamp_;
            ballPos_.x += ballVel_.x * h;
            collideX();
            const float prevY = ballPos_.y;
            ballPos_.y += ballVel_.y * h;
            collideY(prevY);
        }
    }

    void collideX() {
        forge::Circle ball{ballPos_, BALL_R};
        // Bricks first (they resolve to the wall-adjacent edge), then the
        // walls clamp, so wall-overlapping corner hits settle at the wall.
        const float vx = ballVel_.x;
        bool hit = false;
        float edge = 0.0f;
        for (size_t i = 0; i < bricks_.size(); ++i) {
            Brick& b = bricks_[i];
            if (b.hp <= 0 || b.touchStamp == stamp_) continue;
            ball.center = ballPos_;
            if (!forge::circleVsRect(ball, b.rect)) continue;
            b.touchStamp = stamp_;
            if (vx > 0.0f) edge = hit ? std::min(edge, b.rect.x) : b.rect.x;
            else edge = hit ? std::max(edge, b.rect.x + b.rect.w) : b.rect.x + b.rect.w;
            hit = true;
            damageBrick(i);
        }
        if (hit) {
            ballPos_.x = vx > 0.0f ? edge - BALL_R : edge + BALL_R;
            ballVel_.x = -vx;
            sanitizeDirection();
        }
        if (ballPos_.x - BALL_R < WALL) {
            ballPos_.x = WALL + BALL_R;
            ballVel_.x = std::fabs(ballVel_.x);
            hitWallFx({WALL, ballPos_.y});
        }
        if (ballPos_.x + BALL_R > FIELD_W - WALL) {
            ballPos_.x = FIELD_W - WALL - BALL_R;
            ballVel_.x = -std::fabs(ballVel_.x);
            hitWallFx({FIELD_W - WALL, ballPos_.y});
        }
        // Paddle side clip: bounce off the ends instead of sliding under.
        Rect pr = paddleRect();
        ball.center = ballPos_;
        if (forge::circleVsRect(ball, pr)) {
            if (ballPos_.x < paddleX_) {
                ballPos_.x = pr.x - BALL_R;
                ballVel_.x = -std::fabs(ballVel_.x);
            } else {
                ballPos_.x = pr.x + pr.w + BALL_R;
                ballVel_.x = std::fabs(ballVel_.x);
            }
            // A clip at the paddle end can push the ball into a wall; clamp it
            // so the next substep doesn't start out of bounds.
            ballPos_.x = forge::clamp(ballPos_.x, WALL + BALL_R, FIELD_W - WALL - BALL_R);
            sanitizeDirection();
        }
    }

    void collideY(float prevY) {
        const float vy = ballVel_.y;
        if (ballPos_.y + BALL_R > PLAY_TOP) {
            ballPos_.y = PLAY_TOP - BALL_R;
            ballVel_.y = -std::fabs(vy);
            hitWallFx({ballPos_.x, PLAY_TOP});
        }
        bool hit = false;
        float edge = 0.0f;
        forge::Circle ball{ballPos_, BALL_R};
        for (size_t i = 0; i < bricks_.size(); ++i) {
            Brick& b = bricks_[i];
            if (b.hp <= 0 || b.touchStamp == stamp_) continue;
            ball.center = ballPos_;
            if (!forge::circleVsRect(ball, b.rect)) continue;
            b.touchStamp = stamp_;
            if (vy > 0.0f) edge = hit ? std::min(edge, b.rect.y) : b.rect.y;
            else edge = hit ? std::max(edge, b.rect.y + b.rect.h) : b.rect.y + b.rect.h;
            hit = true;
            damageBrick(i);
        }
        if (hit) {
            ballPos_.y = vy > 0.0f ? edge - BALL_R : edge + BALL_R;
            ballVel_.y = -vy;
            sanitizeDirection();
        }
        Rect pr = paddleRect();
        ball.center = ballPos_;
        // One-way platform test: only bounce when the ball was above the
        // paddle top before this substep. (Requiring the ball's center to be
        // above the paddle midline instead lets fast balls sink through.)
        if (ballVel_.y < 0.0f && prevY >= pr.y + pr.h - 0.5f &&
            forge::circleVsRect(ball, pr)) {
            bounceOffPaddle(pr);
        }
        if (ballPos_.y - BALL_R < 0.0f) onBallLost();
    }

    void bounceOffPaddle(const Rect& pr) {
        float rel = forge::clamp((ballPos_.x - paddleX_) / (PADDLE_W * 0.5f), -1.0f, 1.0f);
        float ang = rel * BOUNCE_ANGLE_MAX;
        float speed = currentSpeed();
        // Angle comes from where you hit; the sweep adds sidespin on top.
        ballVel_ = {std::sin(ang) * speed + paddleVelActual_ * PADDLE_ENGLISH * english_,
                    std::cos(ang) * speed};
        sanitizeDirection();
        setSpeed(speed);
        ballPos_.y = pr.y + pr.h + BALL_R + 0.5f;
        paddleFlash_ = 0.1f;
        cam.addTrauma(0.03f);
        playSfx(sfx_.paddle, 0.9f, panFor(ballPos_.x), rng_.range(0.97f, 1.03f));
        forge::ParticleSystem& fx = fxWall();
        fx.setPosition({ballPos_.x, pr.y + pr.h});
        fx.emit(6);
    }

    void damageBrick(size_t idx) {
        Brick& b = bricks_[idx];
        b.flash = 0.1f;
        const int row = (int)(idx / (size_t)COLS);
        Vec2 at = b.rect.center();
        if (--b.hp > 0) {  // armored brick survives one hit
            playSfx(sfx_.armor, 0.85f, panFor(at.x), rng_.range(0.95f, 1.05f));
            fx_[row].setPosition(at);
            fx_[row].emit(5);
            cam.addTrauma(0.02f);
            return;
        }
        --bricksLeft_;
        ++brokenInLevel_;
        score_ += b.points;
        if (score_ > highScore_) { highScore_ = score_; newHigh_ = true; }
        addPopup(at, b.points, b.color);
        playSfx(sfx_.brick[row], 1.0f, panFor(at.x), rng_.range(0.97f, 1.03f));
        fx_[row].setPosition(at);
        fx_[row].emit(14);
        cam.addTrauma(0.05f);
        setSpeed(currentSpeed());  // classic: the ball speeds up as bricks fall
    }

    void hitWallFx(Vec2 at) {
        cam.addTrauma(0.012f);
        playSfx(sfx_.wall, 0.55f, panFor(at.x), rng_.range(0.94f, 1.06f));
        forge::ParticleSystem& fx = fxWall();
        fx.setPosition(at);
        fx.emit(3);
    }

    void addPopup(Vec2 at, int value, Color color) {
        if (popups_.size() >= 24) popups_.erase(popups_.begin());
        popups_.push_back({at, value, 0.7f, color});
    }

    void updatePopups(float dt) {
        for (size_t i = 0; i < popups_.size();) {
            Popup& p = popups_[i];
            p.life -= dt;
            p.pos.y += 34.0f * dt;
            if (p.life <= 0.0f) popups_[i] = popups_.back(), popups_.pop_back();
            else ++i;
        }
    }

    // ------------------------------------------------------------ drawing
    void pushTrail(Vec2 p) {
        trail_[trailHead_] = p;
        trailHead_ = (trailHead_ + 1) % TRAIL_N;
        if (trailCount_ < TRAIL_N) ++trailCount_;
    }

    void drawField(forge::SpriteBatch& b) {
        b.drawRect({WALL, 0.0f, FIELD_W - 2 * WALL, PLAY_TOP}, kPanel);
        b.drawRect({0.0f, 0.0f, WALL, FIELD_H}, kWall);
        b.drawRect({FIELD_W - WALL, 0.0f, WALL, FIELD_H}, kWall);
        b.drawRect({0.0f, PLAY_TOP, FIELD_W, HUD_H}, kHudBar);
        b.drawRect({WALL, 0.0f, 1.5f, PLAY_TOP}, kAccent);
        b.drawRect({FIELD_W - WALL - 1.5f, 0.0f, 1.5f, PLAY_TOP}, kAccent);
        b.drawRect({0.0f, PLAY_TOP - 1.5f, FIELD_W, 1.5f}, kAccent);
        b.drawRect({WALL, 0.0f, FIELD_W - 2 * WALL, 2.5f}, withA(kDanger, 0.6f));
    }

    void drawBricks(forge::SpriteBatch& b) {
        for (const Brick& br : bricks_) {
            if (br.hp <= 0) continue;
            const Rect& r = br.rect;
            Color base = br.maxHp == 2 ? br.color.lerped(kSteel, 0.30f) : br.color;
            b.drawRect(r, shade(base, 0.82f));
            b.drawRect({r.x, r.y + r.h - 4.0f, r.w, 4.0f}, shade(base, 1.25f));
            b.drawRect({r.x, r.y, r.w, 3.0f}, shade(base, 0.55f));
            if (br.maxHp == 2) {
                b.drawRectOutline(r, withA(kSteel, 0.55f), 1.5f);
                if (br.hp == 1) {  // cracked
                    Color crack = withA(Color::black(), 0.6f);
                    b.drawLine({r.x + r.w * 0.22f, r.y + 3.0f},
                               {r.x + r.w * 0.36f, r.y + r.h - 3.0f}, crack, 1.6f);
                    b.drawLine({r.x + r.w * 0.62f, r.y + 3.0f},
                               {r.x + r.w * 0.50f, r.y + r.h - 3.0f}, crack, 1.6f);
                }
            }
            if (br.flash > 0.0f)
                b.drawRect(r, withA(Color::white(), br.flash * 8.0f));
        }
    }

    void drawPaddle(forge::SpriteBatch& b) {
        Rect r = paddleRect();
        const float cap = PADDLE_H * 0.5f;
        b.drawRect({r.x + cap, r.y, r.w - 2 * cap, r.h}, kPaddle);
        b.drawCircle({r.x + cap, r.y + cap}, cap, kPaddle, 12);
        b.drawCircle({r.x + r.w - cap, r.y + cap}, cap, kPaddle, 12);
        b.drawRect({r.x + cap, r.y + r.h - 3.0f, r.w - 2 * cap, 3.0f}, Color::white());
        b.drawRect({paddleX_ - 11.0f, r.y + 3.0f, 22.0f, r.h - 6.0f},
                   withA(kPaddleAccent, 0.85f));
        if (paddleFlash_ > 0.0f)
            b.drawRect(r.expanded(1.0f), withA(Color::white(), paddleFlash_ * 6.0f));
    }

    void drawBall(forge::SpriteBatch& b) {
        if (state_ == State::LifeLost || state_ == State::GameOver ||
            state_ == State::Title || state_ == State::LevelClear)
            return;
        // trail (oldest -> newest)
        for (int i = 0; i < trailCount_; ++i) {
            int idx = (trailHead_ - trailCount_ + i + 2 * TRAIL_N) % TRAIL_N;
            float t = (float)(i + 1) / (float)trailCount_;
            b.drawCircle(trail_[idx], BALL_R * (0.25f + 0.38f * t),
                         withA(Color::white(), 0.13f * t), 12);
        }
        float pulse = ballAttached_ ? 1.0f + 0.08f * std::sin(uiTime_ * 9.0f) : 1.0f;
        b.drawCircle(ballPos_, BALL_R * 2.0f * pulse, withA(Color::white(), 0.07f), 20);
        b.drawCircle(ballPos_, BALL_R * pulse, Color::white(), 20);
        b.drawCircle(ballPos_ + Vec2{-2.0f, 2.2f}, BALL_R * 0.32f,
                     withA(Color::white(), 0.95f), 8);
    }

    void drawPopups(forge::SpriteBatch& b) {
        for (const Popup& p : popups_) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "+%d", p.value);
            float a = forge::clamp01(p.life / 0.7f);
            drawTextCentered(b, buf, p.pos + Vec2{0.0f, 10.0f}, 0.55f,
                             withA(shade(p.color, 1.35f), a));
        }
    }

    void drawHud(forge::SpriteBatch& b) {
        char buf[64];
        const Color dim = withA(Color::white(), 0.45f);
        std::snprintf(buf, sizeof(buf), "%06d", score_);
        drawText(b, "SCORE", {26.0f, FIELD_H - 9.0f}, 0.62f, dim);
        drawText(b, buf, {26.0f + widthOf("SCORE", 0.62f) + 14.0f, FIELD_H - 9.0f},
                 0.62f, Color::white());

        std::snprintf(buf, sizeof(buf), "HIGH %06d", highScore_);
        drawTextCentered(b, buf, {FIELD_W * 0.5f, FIELD_H - 25.0f}, 0.62f,
                         newHigh_ ? kGold : dim);

        std::snprintf(buf, sizeof(buf), "LEVEL %02d", level_);
        drawText(b, buf, {FIELD_W - 26.0f - widthOf(buf, 0.62f), FIELD_H - 9.0f},
                 0.62f, dim);

        for (int i = 0; i < lives_; ++i)  // spare balls
            b.drawCircle({(float)(28 + i * 22), 24.0f}, BALL_R, Color::white(), 14);

        if (state_ == State::Play || state_ == State::Serve)
            drawTextCentered(b, "ESC PAUSE   R RESTART   Q QUIT",
                             {FIELD_W * 0.5f, 26.0f}, 0.5f, withA(Color::white(), 0.22f));
    }

    void drawOverlays(forge::SpriteBatch& b) {
        auto dimField = [&](float a) {
            b.drawRect({WALL, 0.0f, FIELD_W - 2 * WALL, PLAY_TOP},
                       withA(Color::black(), a));
        };
        if (paused_) {
            dimField(0.62f);
            drawBig(b, "PAUSED", 312.0f, 0.85f, Color::white());
            drawTextCentered(b, "ESC RESUME   R RESTART   Q QUIT",
                             {FIELD_W * 0.5f, 246.0f}, 0.62f,
                             withA(Color::white(), 0.7f));
            return;
        }
        switch (state_) {
            case State::Title: {
                dimField(0.74f);
                char buf[64];
                drawBig(b, "BREAKOUT", 358.0f, 1.0f, Color::white());
                drawTextCentered(b, "A FORGE ENGINE DEMO", {FIELD_W * 0.5f, 306.0f},
                                 0.6f, withA(kPaddleAccent, 0.9f));
                std::snprintf(buf, sizeof(buf), "HIGH SCORE  %06d", highScore_);
                drawTextCentered(b, buf, {FIELD_W * 0.5f, 268.0f}, 0.62f, kGold);
                float blink = 0.55f + 0.45f * std::sin(uiTime_ * 5.0f);
                drawTextCentered(b, "PRESS SPACE TO START",
                                 {FIELD_W * 0.5f, 206.0f}, 0.72f,
                                 withA(Color::white(), blink));
                const Color hint = withA(Color::white(), 0.5f);
                drawTextCentered(b, "MOVE   A/D  ARROWS  MOUSE  GAMEPAD",
                                 {FIELD_W * 0.5f, 152.0f}, 0.58f, hint);
                drawTextCentered(b, "LAUNCH SPACE  CLICK  GAMEPAD A",
                                 {FIELD_W * 0.5f, 130.0f}, 0.58f, hint);
                drawTextCentered(b, "PAUSE ESC      QUIT Q",
                                 {FIELD_W * 0.5f, 108.0f}, 0.58f, hint);
                break;
            }
            case State::Serve:
                if (stateTimer_ < 1.4f)
                    drawBig(b, "LEVEL " + std::to_string(level_), 348.0f,
                            0.62f * (1.0f - stateTimer_ / 1.4f) + 0.25f,
                            withA(Color::white(), 1.0f - stateTimer_ / 1.4f));
                if (!autoplay_) {
                    float blink = 0.4f + 0.6f * std::sin(uiTime_ * 6.0f);
                    drawTextCentered(b, "SPACE TO LAUNCH",
                                     {FIELD_W * 0.5f, PADDLE_Y + 104.0f}, 0.62f,
                                     withA(Color::white(), 0.55f * blink + 0.15f));
                }
                break;
            case State::LifeLost: {
                float a = forge::clamp01(1.1f - stateTimer_);
                dimField(0.5f * a);
                drawBig(b, "BALL LOST", 348.0f, 0.7f, withA(Color::fromHex(0xFF6B6BFF), a));
                break;
            }
            case State::LevelClear: {
                dimField(0.35f);
                char buf[64];
                drawBig(b, "LEVEL " + std::to_string(level_) + " CLEARED", 336.0f,
                        0.6f, Color::white());
                std::snprintf(buf, sizeof(buf), "+%d BONUS", clearBonus_);
                drawTextCentered(b, buf, {FIELD_W * 0.5f, 292.0f}, 0.72f, kGold);
                drawTextCentered(b, "NEXT UP: LEVEL " + std::to_string(level_ + 1),
                                 {FIELD_W * 0.5f, 250.0f}, 0.58f,
                                 withA(Color::white(), 0.65f));
                break;
            }
            case State::GameOver: {
                dimField(0.72f);
                char buf[64];
                drawBig(b, "GAME OVER", 348.0f, 0.95f, Color::fromHex(0xFF6B6BFF));
                std::snprintf(buf, sizeof(buf), "SCORE  %06d", score_);
                drawTextCentered(b, buf, {FIELD_W * 0.5f, 282.0f}, 0.72f, Color::white());
                std::snprintf(buf, sizeof(buf), "HIGH   %06d", highScore_);
                drawTextCentered(b, buf, {FIELD_W * 0.5f, 256.0f}, 0.62f,
                                 newHigh_ ? kGold : withA(Color::white(), 0.7f));
                if (newHigh_) {
                    float blink = 0.55f + 0.45f * std::sin(uiTime_ * 5.0f);
                    drawTextCentered(b, "NEW HIGH SCORE!", {FIELD_W * 0.5f, 220.0f},
                                     0.62f, withA(kGold, blink));
                }
                float blink = 0.55f + 0.45f * std::sin(uiTime_ * 5.0f);
                drawTextCentered(b, "PRESS SPACE TO PLAY AGAIN",
                                 {FIELD_W * 0.5f, 170.0f}, 0.68f,
                                 withA(Color::white(), blink));
                break;
            }
            case State::Play:
                break;
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
        forge::SoundHandle wall, paddle, launch, armor, lose, clear, over;
        forge::SoundHandle brick[ROWS];
    };

    forge::Engine* engine_ = nullptr;
    forge::Camera2D cam;
    forge::Font font_;
    forge::Font fontBig_;
    const forge::Font* bigFont_ = nullptr;
    float bigScale_ = 1.0f;
    bool haveBig_ = false;

    forge::ParticleSystem fx_[FX_COUNT];
    Sfx sfx_;

    State state_ = State::Title;
    float stateTimer_ = 0.0f;
    float uiTime_ = 0.0f;
    bool paused_ = false;
    bool autoplay_ = false;
    bool sloppyBot_ = false;
    float english_ = 1.0f;  // --english: scales/flips the sweep -> spin term

    std::vector<Brick> bricks_;
    int bricksLeft_ = 0;
    int brokenInLevel_ = 0;
    int level_ = 1;
    int score_ = 0, highScore_ = 0;
    int lives_ = LIVES_START;
    int clearBonus_ = 0;
    bool newHigh_ = false;

    float paddleX_ = FIELD_W * 0.5f;
    float paddleVel_ = 0.0f;
    float paddleVelActual_ = 0.0f;
    float paddleFlash_ = 0.0f;
    bool mouseActive_ = false;

    bool ballAttached_ = true;
    Vec2 ballPos_{FIELD_W * 0.5f, PADDLE_Y + PADDLE_H + BALL_R + 1.0f};
    Vec2 ballVel_{0.0f, 0.0f};
    int stamp_ = 0;

    static constexpr int TRAIL_N = 18;
    Vec2 trail_[TRAIL_N];
    int trailHead_ = 0, trailCount_ = 0;
    float trailTimer_ = 0.0f;

    std::vector<Popup> popups_;
    forge::Random rng_{20261018};
};

// ------------------------------------------------------------------- util
struct Args {
    std::string shot;
    std::string record;
    int frames = 0;
    uint64_t seed = 20261018;
    int level = 0;
    int pauseAt = 0;                 // --pause-at: ESC tap frame (shot mode)
    int width = 1024, height = 768;  // --shot capture size
    bool title = false;
    bool noAi = false;
    bool sloppy = false;
    float english = 1.0f;            // --english: sweep -> spin scale (<0 inverts)
    bool demo = false;
    bool help = false;
};

Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](void) -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (arg == "--shot") { if (const char* v = next()) a.shot = v; }
        else if (arg == "--record") { if (const char* v = next()) a.record = v; }
        else if (arg == "--frames") { if (const char* v = next()) a.frames = std::atoi(v); }
        else if (arg == "--seed") { if (const char* v = next()) a.seed = std::strtoull(v, nullptr, 10); }
        else if (arg == "--level") { if (const char* v = next()) a.level = std::atoi(v); }
        else if (arg == "--pause-at") { if (const char* v = next()) a.pauseAt = std::atoi(v); }
        else if (arg == "--size") {
            if (const char* v = next()) std::sscanf(v, "%dx%d", &a.width, &a.height);
        }
        else if (arg == "--title") a.title = true;
        else if (arg == "--no-ai") a.noAi = true;
        else if (arg == "--sloppy") a.sloppy = true;
        else if (arg == "--english") { if (const char* v = next()) a.english = (float)std::atof(v); }
        else if (arg == "--demo") a.demo = true;
        else if (arg == "-h" || arg == "--help") a.help = true;
    }
    if (a.width < 64) a.width = 64;
    if (a.height < 64) a.height = 64;
    return a;
}

void printUsage() {
    std::printf(
        "07_breakout — Atari-style Breakout on Forge\n"
        "  A/D, arrows, mouse, gamepad move · SPACE/click/A launch · ESC pause\n"
        "  R restart · Q quit\n"
        "Options:\n"
        "  --shot FILE   render N fixed steps with the autopilot, save PNG, exit\n"
        "  --record FILE record N fixed steps of the autopilot to an MP4 (ffmpeg)\n"
        "  --frames N    frames to simulate/hold before exit (shot/record/CI)\n"
        "  --size WxH    capture size for --shot/--record (default 1024x768)\n"
        "  --level N     start playing at level N (shot mode)\n"
        "  --title       stay on the title screen (shot mode)\n"
        "  --demo        watch the autopilot play (interactive)\n"
        "  --no-ai       disable the autopilot\n"
        "  --sloppy      autopilot plays badly (capture life-lost screens)\n"
        "  --english F   paddle sweep -> ball spin (1 default, 0 off, <0 inverted)\n"
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

    forge::EngineConfig cfg;
    cfg.window.title = "Forge 07_breakout — Atari Breakout";
    cfg.window.width = 1024;
    cfg.window.height = 768;              // 4:3: the playfield fills it
    cfg.window.vsync = args.shot.empty() && args.record.empty();
    cfg.fixedStep = 1.0f / 60.0f;
#if FORGE_PLATFORM_WEB
    // The drawing buffer takes the canvas's CSS size once and then stays put
    // (not resizable); the fitted camera letterboxes the playfield into it.
    cfg.window.resizable = false;
    // Emscripten takes the frame loop over by unwinding main()'s stack, and
    // with wasm exceptions on that unwind runs the destructors of main()'s
    // locals (a unique_ptr owner would free the game under the loop). Statics
    // outlive main and are never destroyed: the web runtime never exits.
    static forge::Engine engine(cfg);
    static Breakout game;
#else
    forge::Engine engine(cfg);
    Breakout game;
#endif
    if (!engine.valid()) return 1;

    game.setSeed(args.seed);
    game.setEnglish(args.english);
    game.init(engine);

#if !FORGE_PLATFORM_WEB
    if (!args.record.empty()) {
        // Video of the autopilot: a beat of the title card, then play. One
        // fixed step per frame, rendered offscreen at --size.
        game.setAutoplay(!args.noAi);
        game.setSloppyBot(args.sloppy);
        const bool ok = clip::recordClip(
            args.record, args.width, args.height, args.frames > 0 ? args.frames : 1200,
            [&] { game.update(1.0f / 60.0f); },
            [&](forge::Framebuffer* fb) { game.renderTo(fb); }, engine);
        return ok && game.healthy() ? 0 : 1;
    }
#endif

    if (!args.shot.empty()) {
        // Deterministic capture: drive the simulation with a fixed dt (no wall
        // clock), render once into a 4:3 offscreen target, read it back.
        const int frames = args.frames > 0 ? args.frames : 1200;
        game.setAutoplay(!args.noAi && !args.title);
        game.setSloppyBot(args.sloppy);
        if (!args.title) game.startRun(args.level > 0 ? args.level : 1);
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
        std::printf("shot: %s (%dx%d, %d frames) score=%06d level=%d lives=%d "
                    "bricks=%d draws=%d sane=%d %s\n",
                    args.shot.c_str(), shotSize.x, shotSize.y, frames, game.score(),
                    game.level(), game.lives(), game.bricksLeft(), engine.drawCalls(),
                    sane ? 1 : 0, ok ? "OK" : "FAILED");
        return ok && sane ? 0 : 1;
    }

#if FORGE_PLATFORM_WEB
    // The page opens on the title card and runs until the tab closes. Nothing
    // to capture: the loop touches only the statics, and main()'s locals are
    // gone by frame one.
    engine.runFixed([](float dt) { game.update(dt); }, [] { game.render(); });
#else
    const bool ci = std::getenv("CI") != nullptr;
    game.setAutoplay((args.demo || ci) ? !args.noAi : false);
    const int frameLimit = args.frames > 0 ? args.frames : (ci ? 600 : 0);
    int frames = 0;
    engine.runFixed(
        [&](float dt) { game.update(dt); },
        [&] {
            game.render();
            if (frameLimit > 0 && ++frames >= frameLimit) engine.quit();
        });
#endif
    std::printf("breakout: score=%06d level=%d bricks_left=%d sane=%d\n",
                game.score(), game.level(), game.bricksLeft(), game.healthy() ? 1 : 0);
    return game.healthy() ? 0 : 1;
}
