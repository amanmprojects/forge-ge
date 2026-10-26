// 09_chess - a complete chess game on Forge.
//
// Rules and bot are the C++ port of amanmprojects/chess (0x88 engine with
// perft-verified move generation + negamax search): castling, en passant,
// promotion, check/checkmate/stalemate, threefold repetition, the fifty-move
// rule and insufficient material are all handled by chess.hpp.
//
//   move    click a piece, then a target - or drag and drop
//   cursor  arrows + Enter (gamepad d-pad + A)
//   keys    U undo - F flip - H hint - N new game - ESC pause - Q quit
//
// The bot thinks on a worker thread, so the board never freezes. Five levels
// weaken it by sampling near-best moves (see chess::ai::levels).
//
// Pieces are 256px sprites from assets/ — one image per board square,
// rasterized from the SVGs of amanmprojects/chess by assets/make_pieces.sh;
// delete that folder and the game falls back to drawing simple silhouettes,
// so it works with no assets at all.
//
// Headless capture:
//   ./09_chess --shot out.png --frames 300 [--moves "e4 e5 Nf3"] [--fen FEN]
//              [--level 3] [--title] [--size 1280x800]
//   ./09_chess --record clip.mp4 --frames 1500     # bot-vs-bot video (ffmpeg)
// CI=1 ./09_chess [--frames 300]      # deterministic bot-vs-bot smoke run
#include <forge/forge.hpp>

#include "ai.hpp"
#include "chess.hpp"
#include "clip.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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

using chess::Move;
using forge::Color;
using forge::Rect;
using forge::Vec2;

namespace ai = chess::ai;

// --------------------------------------------------------------------- layout
// Fixed 1280x800 logical field, letterboxed into any window by a fitted
// Camera2D. All coordinates are field units, Y-up.
constexpr float FIELD_W = 1280.0f, FIELD_H = 800.0f;
constexpr float SQ = 88.0f;
constexpr float BOARD_SIZE = SQ * 8.0f;                 // 704
constexpr float BOARD_X = 40.0f, BOARD_Y = 48.0f;
constexpr float PANEL_X = BOARD_X + BOARD_SIZE + 20.0f; // 764
constexpr float PANEL_W = FIELD_W - PANEL_X - 40.0f;    // 476


// -------------------------------------------------------------------- palette
const Color kBg = Color::fromHex(0x14161CFF);
const Color kPanelBg = Color::fromHex(0x1D2028FF);
const Color kPanelEdge = Color::fromHex(0x2B2F3AFF);
const Color kLightSq = Color::fromHex(0xE9EDD4FF);
const Color kDarkSq = Color::fromHex(0x6F9550FF);
const Color kSelMark = Color::fromHex(0xF2D64BFF);
const Color kLastMove = Color::fromHex(0xF2D64BFF);
const Color kHintMark = Color::fromHex(0x59C7F0FF);
const Color kCheckMark = Color::fromHex(0xE2574CFF);
const Color kWhiteBody = Color::fromHex(0xF7F4EAFF);
const Color kWhiteOutline = Color::fromHex(0x30343EFF);
const Color kBlackBody = Color::fromHex(0x3B3F4CFF);
const Color kBlackOutline = Color::fromHex(0x11131AFF);
const Color kBlackBodyHi = Color::fromHex(0x6A7080FF); // faint highlight for black pieces
const Color kText = Color::fromHex(0xE9ECF2FF);
const Color kTextDim = Color::fromHex(0x99A1B3FF);
const Color kTextFaint = Color::fromHex(0x646D80FF);
const Color kAccent = Color::fromHex(0x6FB1FCFF);
const Color kAccentDim = Color::fromHex(0x33507FFF);
const Color kGood = Color::fromHex(0x86C97EFF);
const Color kBad = Color::fromHex(0xE2574CFF);
const Color kButton = Color::fromHex(0x272B36FF);
const Color kButtonHot = Color::fromHex(0x333949FF);
const Color kButtonOn = Color::fromHex(0x2E4E7DFF);
const Color kCard = Color::fromHex(0x1A1D25F2);

static Color withA(Color c, float a) { return {c.r, c.g, c.b, a}; }
static Color shade(Color c, float f) {
    return {std::min(1.0f, c.r * f), std::min(1.0f, c.g * f), std::min(1.0f, c.b * f), c.a};
}

// ----------------------------------------------------------------- geometry
static Rect squareRect(int sq, bool flip) {
    const int file = chess::fileOf(sq), rank = chess::rankOf(sq);
    const int fx = flip ? 7 - file : file;
    const int fy = flip ? 7 - rank : rank; // from the bottom edge: White at the bottom
    return {BOARD_X + (float)fx * SQ, BOARD_Y + (float)fy * SQ, SQ, SQ};
}
static Vec2 squareCenter(int sq, bool flip) { return squareRect(sq, flip).center(); }

/** Board square under a world position, or -1 outside the board. */
static int squareAtWorld(Vec2 w, bool flip) {
    const float x = w.x - BOARD_X, y = w.y - BOARD_Y;
    if (x < 0.0f || y < 0.0f || x >= BOARD_SIZE || y >= BOARD_SIZE) return -1;
    int file = (int)(x / SQ);
    int row = (int)(y / SQ);
    if (flip) {
        file = 7 - file;
        row = 7 - row;
    }
    return row * 16 + file;
}

// ----------------------------------------------------------- SFX synthesis
// Tiny chiptune synth, same approach as 07_breakout: a chain of notes rendered
// into an in-memory 16-bit mono WAV.
struct Note {
    float f0, f1; // start/end frequency (Hz), swept linearly
    float dur;    // seconds
    float vol;    // 0..1
    int wave;     // 0 sine, 1 square, 2 triangle
    float noise;  // 0..1 noise blend
};

static std::vector<uint8_t> synthWav(const std::vector<Note>& notes, int rate = 22050) {
    std::vector<int16_t> pcm;
    float phase = 0.0f;
    forge::Random rng{0xC0FFEE};
    for (const Note& n : notes) {
        const int frames = std::max(1, (int)(n.dur * (float)rate));
        for (int i = 0; i < frames; ++i) {
            const float u = (float)i / (float)frames;
            const float f = forge::lerp(n.f0, n.f1, u);
            phase += f / (float)rate;
            float s;
            if (n.wave == 1) s = std::sin(forge::TAU * phase) >= 0.0f ? 1.0f : -1.0f;
            else if (n.wave == 2) s = std::asin(std::sin(forge::TAU * phase)) * (2.0f / forge::PI);
            else s = std::sin(forge::TAU * phase);
            s = s * (1.0f - n.noise) + (rng.nextFloat() * 2.0f - 1.0f) * n.noise;
            const float env = std::min(1.0f, (float)i / (0.002f * (float)rate)) *
                              std::exp(-3.0f * u);
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
    w32(16, 16);
    w16(20, 1);
    w16(22, 1);
    w32(24, (uint32_t)rate);
    w32(28, (uint32_t)rate * 2);
    w16(32, 2);
    w16(34, 16);
    std::memcpy(&wav[36], "data", 4);
    w32(40, (uint32_t)bytes);
    std::memcpy(&wav[44], pcm.data(), (size_t)bytes);
    return wav;
}

// ------------------------------------------------- piece drawing (fallback)
// Preferred path: textured 256px sprites from examples/chess/assets (see
// make_pieces.sh), drawn by ChessGame::drawPiece below. These shapes are the
// no-assets fallback: each piece is a slightly larger silhouette in the
// outline colour with the body on top, which gives a clean rim without
// textures.
struct PiecePen {
    forge::SpriteBatch& b;
    Vec2 c;
    float s;
    Color col;

    void rect(float nx, float ny, float w, float h) const {
        b.drawRect(Rect::fromCenter(c + Vec2{nx * s, ny * s}, Vec2{w * s, h * s}), col);
    }
    void circle(float nx, float ny, float r, int seg = 24) const {
        b.drawCircle(c + Vec2{nx * s, ny * s}, r * s, col, seg);
    }
    void line(float x0, float y0, float x1, float y1, float thick) const {
        b.drawLine(c + Vec2{x0 * s, y0 * s}, c + Vec2{x1 * s, y1 * s}, col, thick * s);
    }
};

/** One piece silhouette in normalized units (piece height ≈ 1). */
static void pieceShapes(const PiecePen& p, int type) {
    // Shared pedestal.
    p.rect(0.0f, -0.44f, 0.58f, 0.08f);
    p.rect(0.0f, -0.37f, 0.40f, 0.06f);

    switch (type) {
        case chess::PAWN:
            p.line(-0.12f, 0.08f, -0.19f, -0.32f, 0.15f);
            p.line(0.12f, 0.08f, 0.19f, -0.32f, 0.15f);
            p.rect(0.0f, -0.20f, 0.20f, 0.26f);
            p.rect(0.0f, 0.13f, 0.22f, 0.05f);
            p.circle(0.0f, 0.28f, 0.135f);
            break;
        case chess::KNIGHT:
            p.rect(0.0f, -0.16f, 0.30f, 0.32f);
            p.line(-0.09f, -0.32f, -0.18f, 0.02f, 0.13f);
            p.line(0.09f, -0.32f, 0.16f, 0.02f, 0.13f);
            p.circle(0.03f, 0.16f, 0.155f);
            p.line(0.06f, 0.28f, -0.20f, 0.13f, 0.145f); // muzzle
            p.line(0.09f, 0.28f, 0.12f, 0.44f, 0.07f);   // ear
            break;
        case chess::ROOK:
            p.rect(0.0f, -0.12f, 0.36f, 0.40f);
            p.rect(0.0f, 0.12f, 0.50f, 0.09f);
            p.rect(-0.18f, 0.25f, 0.13f, 0.18f);
            p.rect(0.0f, 0.25f, 0.13f, 0.18f);
            p.rect(0.18f, 0.25f, 0.13f, 0.18f);
            break;
        case chess::BISHOP:
            p.line(-0.11f, 0.06f, -0.18f, -0.32f, 0.14f);
            p.line(0.11f, 0.06f, 0.18f, -0.32f, 0.14f);
            p.rect(0.0f, -0.20f, 0.18f, 0.26f);
            p.rect(0.0f, 0.10f, 0.28f, 0.06f);
            p.circle(0.0f, 0.24f, 0.13f);
            p.circle(0.0f, 0.40f, 0.05f);
            break;
        case chess::QUEEN: {
            p.line(-0.12f, 0.06f, -0.20f, -0.32f, 0.15f);
            p.line(0.12f, 0.06f, 0.20f, -0.32f, 0.15f);
            p.rect(0.0f, -0.20f, 0.20f, 0.26f);
            p.rect(0.0f, 0.10f, 0.32f, 0.07f);
            p.rect(0.0f, 0.21f, 0.46f, 0.08f);
            const float xs[5] = {-0.20f, -0.10f, 0.0f, 0.10f, 0.20f};
            for (float x : xs) p.circle(x, 0.33f, 0.05f, 16);
            p.circle(0.0f, 0.41f, 0.06f, 16);
            break;
        }
        case chess::KING:
            p.line(-0.12f, 0.06f, -0.20f, -0.32f, 0.15f);
            p.line(0.12f, 0.06f, 0.20f, -0.32f, 0.15f);
            p.rect(0.0f, -0.20f, 0.20f, 0.26f);
            p.rect(0.0f, 0.10f, 0.34f, 0.07f);
            p.rect(0.0f, 0.34f, 0.08f, 0.36f);
            p.rect(0.0f, 0.40f, 0.26f, 0.08f);
            break;
        default:
            break;
    }
}

/** Small internal marks, drawn after the body fill. */
static void pieceDetails(const PiecePen& p, int type) {
    switch (type) {
        case chess::KNIGHT:
            p.circle(0.03f, 0.21f, 0.028f, 10); // eye
            break;
        case chess::BISHOP:
            p.line(-0.055f, 0.30f, 0.045f, 0.18f, 0.035f); // mitre slit
            break;
        default:
            break;
    }
}

static void drawPieceProcedural(forge::SpriteBatch& b, int type, int color, Vec2 center,
                                float size) {
    if (type == chess::EMPTY) return;
    const bool white = color == chess::WHITE;
    const Color body = white ? kWhiteBody : kBlackBody;
    const Color outline = white ? kWhiteOutline : kBlackOutline;
    if (!white) {
        // A faint light rim keeps black pieces readable on dark squares.
        PiecePen pen{b, center, size * 1.14f, withA(shade(outline, 3.0f), 0.85f)};
        pieceShapes(pen, type);
    }
    PiecePen halo{b, center, size * 1.10f, outline};
    pieceShapes(halo, type);
    PiecePen fill{b, center, size, body};
    pieceShapes(fill, type);
    PiecePen detail{b, center, size, outline};
    pieceDetails(detail, type);
}

// ------------------------------------------------------------------ bot
// The search runs on a worker thread: the main thread keeps rendering while
// the bot thinks. Only engine-free code (chess.hpp/ai.hpp) runs in the thread.
class AsyncBot {
public:
    AsyncBot() : search_(size_t(1) << 18) {}
    ~AsyncBot() { cancel(); }

    AsyncBot(const AsyncBot&) = delete;
    AsyncBot& operator=(const AsyncBot&) = delete;

    /** Start thinking about `pos` (copied) at `level`. */
    void start(const chess::Position& pos, const ai::Level& level, uint64_t seed) {
        cancel();
        finished_.store(false, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            result_ = {};
            hasResult_ = false;
        }
        // The thread owns its own copy of the position: no aliasing with the
        // game's board, and no reference to a captured-const object.
        std::unique_ptr<chess::Position> work = std::make_unique<chess::Position>(pos);
        thread_ = std::thread([this, work = std::move(work), level, seed]() {
            const ai::Result r = search_.chooseMove(
                *work, level, seed, [this](const ai::Result& it) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    progress_ = it;
                });
            std::lock_guard<std::mutex> lock(mutex_);
            result_ = r;
            hasResult_ = true;
            finished_.store(true, std::memory_order_relaxed);
        });
    }

    /** Search synchronously on the calling thread (deterministic captures). */
    ai::Result thinkSync(chess::Position& pos, const ai::Level& level, uint64_t seed) {
        cancel();
        const ai::Result r = search_.chooseMove(pos, level, seed);
        std::lock_guard<std::mutex> lock(mutex_);
        result_ = r;
        hasResult_ = true;
        progress_ = r;
        return r;
    }

    bool thinking() const { return thread_.joinable(); }
    bool ready() const { return finished_.load(std::memory_order_relaxed); }

    /** Latest completed iteration (safe to call while thinking). */
    ai::Result progress() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return progress_;
    }

    /** Join and hand over the result. Only call when ready(). */
    ai::Result take() {
        if (thread_.joinable()) thread_.join();
        std::lock_guard<std::mutex> lock(mutex_);
        hasResult_ = false;
        return result_;
    }

    void clearResult() {
        std::lock_guard<std::mutex> lock(mutex_);
        hasResult_ = false;
    }

    /** Drop everything learned so far - call when a new game starts. */
    void reset() {
        cancel();
        search_.newGame();
        clearResult();
        finished_.store(false, std::memory_order_relaxed);
    }

    bool hasResult() const { return ready(); }

    /** Abort any running search and join. */
    void cancel() {
        if (thread_.joinable()) {
            search_.stop();
            thread_.join();
        }
    }

private:
    ai::Search search_;
    mutable std::mutex mutex_;
    ai::Result result_{};
    ai::Result progress_{};
    bool hasResult_ = false;
    std::atomic<bool> finished_{false};
    std::thread thread_;
};

// ------------------------------------------------------------------- state
// One record per played move. SAN has to be generated while the position
// still holds, so it is stored alongside the move.
struct PlayedMove {
    chess::Move move = chess::MOVE_NONE;
    std::string san;
};

struct Sfx {
    forge::SoundHandle move, capture, castle, promote, check, end, ui;
};

// Panel geometry (field units, Y-up): four stacked cards on the right.
constexpr float CARD_STATUS_Y = 628.0f, CARD_STATUS_H = 124.0f;
constexpr float CARD_MOVES_Y = 160.0f, CARD_MOVES_H = 460.0f;
constexpr float CARD_TAKEN_Y = 100.0f, CARD_TAKEN_H = 56.0f;
constexpr float BTN_Y = 48.0f, BTN_H = 44.0f;
constexpr float MOVE_ROW_H = 27.0f;
constexpr int MOVE_ROWS = 16; // per column; the list has two columns

constexpr int kPieceValue[7] = {0, 1, 3, 3, 5, 9, 0};

class ChessGame {
public:
    // ---------------------------------------------------------------- setup
    void init(forge::Engine& engine) {
        engine_ = &engine;
        loadFont(fontBig_, 54.0f, "FORGE_FONT_TITLE");
        loadFont(font_, 24.0f, "FORGE_FONT");
        loadFont(fontSmall_, 18.0f, "FORGE_FONT_SMALL");
        loadPieceTextures();
        initAudio();
#if FORGE_PLATFORM_WEB
        // No worker threads in the browser (they need SharedArrayBuffer and
        // cross-origin isolation): the bot searches inline, paced in update().
        syncBot_ = true;
        botPace_ = 0.6f;
#endif
        fitCamera(engine.viewport());
        mode_ = startInTitle_ ? Mode::Title : Mode::Play;
        if (mode_ == Mode::Title) attract_ = true;
        newGame(chess::START_FEN);
    }

    void setLevel(int l) { level_ = std::clamp(l, 1, (int)ai::levels().size()); }
    void setSeed(uint64_t s) { seed_ = s; }
    void setAttract(bool on) { attract_ = on; }
    void setSyncBot(bool on) { syncBot_ = on; }
    void setBotPace(float seconds) { botPace_ = seconds; }
    void setStartInTitle(bool on) { startInTitle_ = on; }
    void setFlip(bool on) { flip_ = on; }

    /** Reset to `fen` (the standard start when empty). False if it is invalid. */
    bool newGame(const std::string& fen) {
        bot_.reset();
        botState_ = BotState::Idle;
        hintSearch_.newGame();
        if (!pos_.load(fen)) return false;
        records_.clear();
        selSq_ = -1;
        selMoves_ = chess::MoveList{};
        dragging_ = false;
        dragSq_ = -1;
        promoOpen_ = false;
        lastMove_ = chess::MOVE_NONE;
        hintMove_ = chess::MOVE_NONE;
        hintTimer_ = 0.0f;
        paused_ = false;
        gameOverTimer_ = 0.0f;
        moveScroll_ = 0;
        cursorSq_ = chess::squareFromAlgebraic("e2");
        refreshStatus();
        return true;
    }
    bool newGame() { return newGame(chess::START_FEN); }

    /** Play a whitespace-separated list of SAN/UCI moves (scripted captures). */
    bool playScripted(const std::string& moves) {
        std::istringstream in(moves);
        std::string token;
        while (in >> token)
            if (!tryPlayText(token)) return false;
        return true;
    }

    // ---------------------------------------------------------------- loop
    void update(float dt) {
        uiTime_ += dt;
        // The inline bot search returns instantly, so the web build and
        // --record hold it back a beat: without this the attract game would
        // play one move per frame (and the bot would answer before you see
        // your own move). --shot and CI leave botPace_ at 0.
        if (syncPace_ > 0.0f) syncPace_ -= dt;
        auto& in = engine_->input();
        const forge::IVec2 vp = engine_->viewport();
        fitCamera(vp);
        mouse_ = cam_.screenToWorld(in.mousePos(), vp.x, vp.y);
        hoverSq_ = squareAtWorld(mouse_, flip_);

        if (hintTimer_ > 0.0f) {
            hintTimer_ -= dt;
            if (hintTimer_ <= 0.0f) hintMove_ = chess::MOVE_NONE;
        }
        if (gameOver_) gameOverTimer_ += dt;
        cam_.update(dt);

        handleGlobalInput(in);

        const forge::Vec2 wheel = in.wheel();
        if (wheel.y != 0.0f) {
            moveScroll_ += wheel.y > 0.0f ? 1 : -1;
            clampScroll();
        }

        if (mode_ == Mode::Title) {
            if (confirmed(in)) {
                mode_ = Mode::Play;
                attract_ = false;
                newGame(chess::START_FEN);
                playSfx(sfx_.ui);
            } else {
                botPump(); // the attract game keeps running behind the card
            }
            return;
        }
        if (paused_) return;
        if (promoOpen_) {
            updatePromo(in);
            return;
        }

        handlePanelMouse(in);
        if (humansTurn()) {
            handleKeyboard(in);
            handleBoardMouse(in);
        }
        if (!gameOver_) botPump();
    }

    void render() { renderTo(nullptr); }

    /** Synthetic tap on a board square — the deterministic capture path. */
    void injectClick(int sq) {
        auto& in = engine_->input();
        const forge::IVec2 vp = engine_->viewport();
        cam_.update(10.0f); // let any shake settle: the mapping must be exact
        fitCamera(vp);
        const forge::Vec2 screen = cam_.worldToScreen(squareCenter(sq, flip_), vp.x, vp.y);
        in.beginFrame();
        in.setMousePos(screen);
        in.setMouseButton(forge::MouseButton::Left, true);
        update(1.0f / 60.0f);
        in.beginFrame();
        in.setMouseButton(forge::MouseButton::Left, false);
        update(1.0f / 60.0f);
    }

    /** Synthetic press-drag-release from one square to another. */
    void injectDrag(int from, int to) {
        auto& in = engine_->input();
        const forge::IVec2 vp = engine_->viewport();
        cam_.update(10.0f);
        fitCamera(vp);
        const forge::Vec2 a = cam_.worldToScreen(squareCenter(from, flip_), vp.x, vp.y);
        const forge::Vec2 b = cam_.worldToScreen(squareCenter(to, flip_), vp.x, vp.y);
        in.beginFrame();
        in.setMousePos(a);
        in.setMouseButton(forge::MouseButton::Left, true);
        update(1.0f / 60.0f);
        in.beginFrame();
        in.setMousePos(b);
        update(1.0f / 60.0f); // still held: the piece follows the pointer
        in.beginFrame();
        in.setMouseButton(forge::MouseButton::Left, false);
        update(1.0f / 60.0f);
    }

    /** Synthetic key tap (ESC pause, H hint, Q promotion choice, …). */
    void injectKey(forge::Key key) {
        auto& in = engine_->input();
        in.beginFrame();
        in.setKey(key, true);
        update(1.0f / 60.0f);
        in.beginFrame();
        in.setKey(key, false);
        update(1.0f / 60.0f);
    }

    /** fb == nullptr renders into the window; a target renders offscreen. */
    void renderTo(forge::Framebuffer* fb) {
        const forge::IVec2 vp =
            fb ? forge::IVec2{fb->width(), fb->height()} : engine_->viewport();
        if (fb) fb->bind();
        forge::gl::glViewport(0, 0, vp.x, vp.y);
        forge::gl::glClearColor(kBg.r, kBg.g, kBg.b, 1.0f);
        forge::gl::glClear(GL_COLOR_BUFFER_BIT);
        fitCamera(vp);
        auto& b = engine_->batch();
        b.begin(cam_.viewProj(vp.x, vp.y));
        drawBoard(b);
        drawPanel(b);
        drawBands(b);
        if (promoOpen_) drawPromo(b);
        if (mode_ == Mode::Title) drawTitle(b);
        if (paused_ && mode_ == Mode::Play) drawPause(b);
        if (gameOver_ && !paused_ && mode_ != Mode::Title) drawGameOver(b);
        b.end();
        if (fb) forge::Framebuffer::unbind(engine_->viewport().x, engine_->viewport().y);
    }

    // ------------------------------------------------------------ reporting
    /** Cheap invariants for CI: hashes agree, status matches the flag. */
    bool healthy() {
        chess::Position copy(pos_.fen());
        if (copy.hash() != pos_.hash()) return false;
        if (gameOver_ != pos_.status().over) return false;
        return gameOver_ || !pos_.generateMoves().empty();
    }

    std::string resultText() const {
        if (!gameOver_) return "in progress";
        const std::string why = status_.reason ? status_.reason : "";
        const std::string tail = why.empty() ? "" : " - " + why;
        if (winner_ == chess::WHITE) return "white wins" + tail;
        if (winner_ == chess::BLACK) return "black wins" + tail;
        return "draw" + tail;
    }
    int plyCount() const { return (int)records_.size(); }
    int levelIndex() const { return level_; }
    bool over() const { return gameOver_; }

private:
    enum class Mode { Play, Title };
    enum class BotState { Idle, Thinking };

    // ---------------------------------------------------------------- input
    bool confirmed(forge::Input& in) const {
        return in.pressed(forge::Key::Enter) || in.pressed(forge::Key::Space) ||
               in.mousePressed(forge::MouseButton::Left) ||
               in.padPressed(0, forge::GamepadButton::A);
    }

    void handleGlobalInput(forge::Input& in) {
        if (in.pressed(forge::Key::Q)) {
            engine_->quit();
            return;
        }
        if (in.pressed(forge::Key::F) || in.padPressed(0, forge::GamepadButton::Y)) {
            flip_ = !flip_;
            playSfx(sfx_.ui, 0.3f);
        }
        if (in.pressed(forge::Key::N) || in.padPressed(0, forge::GamepadButton::Back)) {
            newGame();
            playSfx(sfx_.ui);
        }
        for (int i = 0; i < 5; ++i)
            if (in.pressed((forge::Key)((int)forge::Key::N1 + i))) {
                setLevel(i + 1);
                playSfx(sfx_.ui, 0.4f);
            }
        if (in.padPressed(0, forge::GamepadButton::Start) && mode_ == Mode::Play &&
            !gameOver_) {
            paused_ = !paused_;
            playSfx(sfx_.ui);
        }
        if (in.pressed(forge::Key::Escape)) {
            if (promoOpen_) {
                promoOpen_ = false;
                playSfx(sfx_.ui);
            } else if (selSq_ >= 0) {
                deselect();
            } else if (mode_ == Mode::Play && !gameOver_) {
                paused_ = !paused_;
                playSfx(sfx_.ui);
            }
        }
        if (mode_ == Mode::Play && !attract_ && !paused_) {
            if (in.pressed(forge::Key::U) || in.padPressed(0, forge::GamepadButton::B)) undo();
            if (in.pressed(forge::Key::H) || in.padPressed(0, forge::GamepadButton::X)) hint();
        }
    }

    void handlePanelMouse(forge::Input& in) {
        if (!in.mousePressed(forge::MouseButton::Left)) return;
        for (int i = 0; i < 5; ++i)
            if (levelPillRect(i).contains(mouse_)) {
                setLevel(i + 1);
                playSfx(sfx_.ui, 0.4f);
            }
        for (int i = 0; i < 4; ++i) {
            if (!buttonRect(i).contains(mouse_)) continue;
            if (i == 0) newGame();
            else if (i == 1) undo();
            else if (i == 2) hint();
            else flip_ = !flip_;
            playSfx(sfx_.ui);
        }
    }

    void handleBoardMouse(forge::Input& in) {
        const bool down = in.mouseDown(forge::MouseButton::Left);
        if (in.mousePressed(forge::MouseButton::Right)) {
            deselect();
            return;
        }
        if (in.mousePressed(forge::MouseButton::Left)) {
            if (hoverSq_ >= 0 && ownsMovePiece(hoverSq_)) {
                select(hoverSq_); // pick the piece up; a drag may follow
                dragging_ = true;
                dragSq_ = hoverSq_;
                dragPos_ = mouse_;
            } else if (hoverSq_ >= 0 && selSq_ >= 0) {
                tryMoveTo(hoverSq_);
            } else {
                deselect();
            }
        }
        if (dragging_ && down) dragPos_ = mouse_;
        if (in.mouseReleased(forge::MouseButton::Left) && dragging_) {
            dragging_ = false;
            dragSq_ = -1;
            if (hoverSq_ >= 0 && selSq_ >= 0) tryMoveTo(hoverSq_);
        }
    }

    void handleKeyboard(forge::Input& in) {
        int dx = 0, dy = 0;
        if (in.pressed(forge::Key::Left) || in.padPressed(0, forge::GamepadButton::DpadLeft)) dx = -1;
        if (in.pressed(forge::Key::Right) || in.padPressed(0, forge::GamepadButton::DpadRight)) dx = 1;
        if (in.pressed(forge::Key::Up) || in.padPressed(0, forge::GamepadButton::DpadUp)) dy = 1;
        if (in.pressed(forge::Key::Down) || in.padPressed(0, forge::GamepadButton::DpadDown)) dy = -1;
        if (dx != 0 || dy != 0) {
            if (flip_) {
                dx = -dx; // the cursor moves the way the board looks
                dy = -dy;
            }
            const int file = std::clamp(chess::fileOf(cursorSq_) + dx, 0, 7);
            const int rank = std::clamp(chess::rankOf(cursorSq_) + dy, 0, 7);
            cursorSq_ = rank * 16 + file;
            playSfx(sfx_.ui, 0.2f);
        }
        if (in.pressed(forge::Key::Enter) || in.pressed(forge::Key::Space) ||
            in.padPressed(0, forge::GamepadButton::A))
            confirmCursor();
    }

    void confirmCursor() {
        if (selSq_ < 0) {
            select(cursorSq_);
            return;
        }
        tryMoveTo(cursorSq_);
    }

    void updatePromo(forge::Input& in) {
        const forge::Key keys[4] = {forge::Key::Q, forge::Key::R, forge::Key::B,
                                    forge::Key::N};
        for (int i = 0; i < 4; ++i)
            if (in.pressed(keys[i])) {
                promoIndex_ = i;
                applyPromo();
                return;
            }
        if (in.pressed(forge::Key::Left)) promoIndex_ = (promoIndex_ + 3) % 4;
        if (in.pressed(forge::Key::Right)) promoIndex_ = (promoIndex_ + 1) % 4;
        if (in.pressed(forge::Key::Enter) || in.pressed(forge::Key::Space) ||
            in.padPressed(0, forge::GamepadButton::A)) {
            applyPromo();
            return;
        }
        if (in.mousePressed(forge::MouseButton::Left)) {
            for (int i = 0; i < 4; ++i)
                if (promoButtonRect(i).contains(mouse_)) {
                    promoIndex_ = i;
                    applyPromo();
                    return;
                }
        }
        if (in.mousePressed(forge::MouseButton::Right)) promoOpen_ = false;
    }

    void applyPromo() {
        // promoMoves_ holds the promotions to one target, in Q/R/B/N order —
        // the same order the dialog draws.
        if (promoIndex_ < 0 || promoIndex_ >= promoMoves_.size()) {
            promoOpen_ = false;
            return;
        }
        const chess::Move m = promoMoves_[promoIndex_];
        promoOpen_ = false;
        applyMove(m);
    }

    // -------------------------------------------------------------- actions
    bool tryPlayText(const std::string& text) {
        const chess::Move m = pos_.resolve(text);
        if (m == chess::MOVE_NONE) return false;
        applyMove(m);
        return true;
    }

    void applyMove(chess::Move move) {
        const std::string san = pos_.toSan(move);
        const int flags = chess::moveFlags(move);
        chess::Move played = chess::MOVE_NONE;
        if (!pos_.playMove(move, &played)) return;

        records_.push_back(PlayedMove{played, san});
        lastMove_ = played;
        selSq_ = -1;
        selMoves_ = chess::MoveList{};
        dragging_ = false;
        dragSq_ = -1;
        hintMove_ = chess::MOVE_NONE;
        hintTimer_ = 0.0f;
        moveScroll_ = 0;

        const float pan = forge::clamp(((float)chess::fileOf(chess::moveTo(played)) - 3.5f) / 3.5f,
                                       -1.0f, 1.0f) * 0.45f;
        if (flags & (chess::FLAG_KCASTLE | chess::FLAG_QCASTLE)) playSfx(sfx_.castle, pan);
        else if (flags & chess::FLAG_PROMO) playSfx(sfx_.promote, pan);
        else if (flags & chess::FLAG_CAPTURE) playSfx(sfx_.capture, pan);
        else playSfx(sfx_.move, pan);

        refreshStatus();
        if (gameOver_) cam_.addTrauma(0.35f);
        else if (pos_.inCheck()) {
            playSfx(sfx_.check);
            cam_.addTrauma(0.14f);
        }
    }

    void undo() {
        if (records_.empty() || mode_ != Mode::Play || attract_) return;
        bot_.cancel();
        botState_ = BotState::Idle;
        pos_.undoPlayed();
        records_.pop_back();
        if (!records_.empty() && pos_.turn() != humanSide_) {
            pos_.undoPlayed(); // take the bot's reply back too
            records_.pop_back();
        }
        lastMove_ = records_.empty() ? chess::MOVE_NONE : records_.back().move;
        selSq_ = -1;
        selMoves_ = chess::MoveList{};
        dragging_ = false;
        dragSq_ = -1;
        promoOpen_ = false;
        hintMove_ = chess::MOVE_NONE;
        hintTimer_ = 0.0f;
        moveScroll_ = 0;
        refreshStatus();
        playSfx(sfx_.ui);
    }

    void hint() {
        if (gameOver_ || attract_ || mode_ != Mode::Play || pos_.turn() != humanSide_) return;
        ai::Level level = ai::levels()[(size_t)(level_ - 1)];
        level.depth = std::min(level.depth, 8);
        level.nodes = std::min<uint64_t>(level.nodes, 300000);
        level.movetimeMs = 0.0;
        level.randomness = 0;
        const ai::Result r = hintSearch_.chooseMove(pos_, level, 0x1DEAull);
        if (r.move == chess::MOVE_NONE) return;
        hintMove_ = r.move;
        hintSan_ = pos_.toSan(r.move); // cached: drawing is const-correct this way
        hintTimer_ = 4.5f;
        cursorSq_ = chess::moveFrom(hintMove_);
        playSfx(sfx_.ui, 0.5f);
    }

    void select(int sq) {
        if (!ownsMovePiece(sq)) return;
        selSq_ = sq;
        selMoves_ = pos_.generateMoves(true, sq);
        cursorSq_ = sq;
    }

    void deselect() {
        selSq_ = -1;
        selMoves_ = chess::MoveList{};
        dragging_ = false;
        dragSq_ = -1;
    }

    void tryMoveTo(int sq) {
        chess::MoveList hits;
        for (int i = 0; i < selMoves_.size(); ++i)
            if (chess::moveTo(selMoves_[i]) == sq) hits.push(selMoves_[i]);
        if (hits.empty()) {
            if (ownsMovePiece(sq)) select(sq); // re-target, or drop the selection
            else deselect();
            return;
        }
        if (hits.size() == 1) {
            applyMove(hits[0]);
            return;
        }
        promoOpen_ = true; // four promotions to the same square
        promoMoves_ = hits;
        promoIndex_ = 0;
        playSfx(sfx_.ui);
    }

    // ------------------------------------------------------------------ bot
    bool humansTurn() const {
        return mode_ == Mode::Play && !attract_ && !gameOver_ && !paused_ &&
               pos_.turn() == humanSide_;
    }
    bool ownsMovePiece(int sq) const {
        if (sq < 0 || !chess::onBoard(sq)) return false;
        const int p = pos_.at(sq);
        return p != chess::EMPTY && chess::colorOf(p) == pos_.turn();
    }
    bool botToMove() const {
        if (gameOver_ || mode_ == Mode::Title) return mode_ == Mode::Title;
        return attract_ || pos_.turn() != humanSide_;
    }

    void botPump() {
        if (syncPace_ > 0.0f) return; // still "thinking" (see update())
        if (!botToMove()) return;
        const ai::Level& level = ai::levels()[(size_t)(level_ - 1)];
        const uint64_t moveSeed = seed_ + (uint64_t)records_.size() * 7919ull;

        if (syncBot_) {
            // Captures run on this thread with the wall clock switched off, so
            // --shot and CI produce the same game every run.
            ai::Level fixed = level;
            fixed.movetimeMs = 0.0;
            const ai::Result r = bot_.thinkSync(pos_, fixed, moveSeed);
            if (r.move != chess::MOVE_NONE) applyMove(r.move);
            syncPace_ = botPace_;
            return;
        }
        if (botState_ == BotState::Idle) {
            botState_ = BotState::Thinking;
            bot_.start(pos_, level, moveSeed);
        } else if (bot_.ready()) {
            const ai::Result r = bot_.take();
            botState_ = BotState::Idle;
            if (r.move != chess::MOVE_NONE) applyMove(r.move);
        }
    }

    void refreshStatus() {
        status_ = pos_.status();
        winner_ = -1;
        if (status_.result) {
            if (std::strcmp(status_.result, "1-0") == 0) winner_ = chess::WHITE;
            else if (std::strcmp(status_.result, "0-1") == 0) winner_ = chess::BLACK;
        }
        gameOver_ = status_.over;
        if (gameOver_) {
            gameOverTimer_ = 0.0f;
            bot_.cancel();
            botState_ = BotState::Idle;
            playSfx(sfx_.end);
        }
    }

    std::string gameOverHeadline() const {
        if (!status_.reason) return "Game over";
        const std::string reason = status_.reason;
        if (reason == "checkmate") return "Checkmate";
        if (reason == "stalemate") return "Stalemate";
        if (reason == "insufficient material") return "Insufficient material";
        if (reason == "fifty-move rule") return "Fifty-move rule";
        if (reason == "threefold repetition") return "Threefold repetition";
        return "Game over";
    }

    std::string botInfoLine() const {
        const std::string name = ai::levels()[(size_t)(level_ - 1)].name;
        if (botState_ == BotState::Thinking) {
            const ai::Result p = bot_.progress();
            std::string s = name;
            if (p.depth > 0)
                s += " - d" + std::to_string(p.depth) + " - " + nodeCount(p.nodes) + " nodes";
            else
                s += " - thinking";
            return s;
        }
        return "Level " + std::to_string(level_) + " - " + name;
    }

    // -------------------------------------------------------------- drawing
    void drawBoard(forge::SpriteBatch& b) const {
        const forge::Rect frame{BOARD_X - 8.0f, BOARD_Y - 8.0f, BOARD_SIZE + 16.0f,
                                BOARD_SIZE + 16.0f};
        b.drawRect(frame, kPanelBg);
        b.drawRectOutline(frame, kPanelEdge, 2.0f);

        for (int sq = 0; sq < 128; ++sq) {
            if (sq & 0x88) {
                sq += 7;
                continue;
            }
            b.drawRect(squareRect(sq, flip_), chess::isLightSquare(sq) ? kLightSq : kDarkSq);
        }

        // Highlights live under the pieces.
        if (lastMove_ != chess::MOVE_NONE) {
            b.drawRect(squareRect(chess::moveFrom(lastMove_), flip_), withA(kLastMove, 0.30f));
            b.drawRect(squareRect(chess::moveTo(lastMove_), flip_), withA(kLastMove, 0.30f));
        }
        if (pos_.inCheck()) {
            const int king = pos_.kingSquare(pos_.turn());
            if (king >= 0) b.drawRect(squareRect(king, flip_), withA(kCheckMark, 0.40f));
        }
        if (selSq_ >= 0) b.drawRect(squareRect(selSq_, flip_), withA(kSelMark, 0.26f));
        if (hoverSq_ >= 0 && humansTurn() && !dragging_ && hoverSq_ != selSq_)
            b.drawRect(squareRect(hoverSq_, flip_), withA(kAccent, 0.10f));

        // Destinations: a dot when empty, a ring on a capture.
        for (int i = 0; i < selMoves_.size(); ++i) {
            const chess::Move m = selMoves_[i];
            const forge::Rect r = squareRect(chess::moveTo(m), flip_);
            if (chess::moveFlags(m) & chess::FLAG_CAPTURE)
                b.drawCircleOutline(r.center(), SQ * 0.42f, withA(kSelMark, 0.55f), 3.0f, 24);
            else
                b.drawCircle(r.center(), SQ * 0.11f, withA(kSelMark, 0.45f), 14);
        }

        for (int sq = 0; sq < 128; ++sq) {
            if (sq & 0x88) {
                sq += 7;
                continue;
            }
            const int p = pos_.at(sq);
            if (p == chess::EMPTY) continue;
            if (dragging_ && sq == dragSq_) continue;
            drawPiece(b, chess::typeOf(p), chess::colorOf(p), squareCenter(sq, flip_), SQ);
        }
        if (dragging_ && dragSq_ >= 0) {
            const int p = pos_.at(dragSq_);
            if (p != chess::EMPTY)
                drawPiece(b, chess::typeOf(p), chess::colorOf(p), dragPos_, SQ);
        }

        if (hintMove_ != chess::MOVE_NONE)
            drawArrow(b, squareCenter(chess::moveFrom(hintMove_), flip_),
                      squareCenter(chess::moveTo(hintMove_), flip_), withA(kHintMark, 0.9f));

        if (humansTurn() && !promoOpen_)
            b.drawRectOutline(squareRect(cursorSq_, flip_), withA(kAccent, 0.7f), 3.0f);

        // Coordinates around the board.
        for (int f = 0; f < 8; ++f) {
            const forge::Rect r = squareRect(flip_ ? 7 - f : f, flip_);
            smallTextCentered(b, std::string(1, (char)('a' + f)), {r.center().x, 26.0f}, 0.8f,
                              kTextFaint);
        }
        for (int rank = 0; rank < 8; ++rank) {
            const forge::Rect r = squareRect(rank * 16, flip_);
            smallTextCentered(b, std::to_string(rank + 1), {BOARD_X - 22.0f, r.center().y}, 0.8f,
                              kTextFaint);
        }
    }

    void drawPanel(forge::SpriteBatch& b) const {
        card(b, {PANEL_X, CARD_MOVES_Y, PANEL_W, CARD_MOVES_H});
        card(b, {PANEL_X, CARD_TAKEN_Y, PANEL_W, CARD_TAKEN_H});
        card(b, {PANEL_X, CARD_STATUS_Y, PANEL_W, CARD_STATUS_H});
        drawMoveList(b);
        drawCaptured(b);
        drawStatus(b);
        drawButtons(b);
    }

    void drawStatus(forge::SpriteBatch& b) const {
        std::string line1, line2;
        forge::Color color1 = kText;
        if (mode_ == Mode::Title) {
            line1 = "Attract mode";
            line2 = "bot vs bot - press Enter to play";
        } else if (gameOver_) {
            line1 = gameOverHeadline();
            line2 = resultText();
            color1 = winner_ == humanSide_ ? kGood : (winner_ < 0 ? kText : kBad);
        } else if (attract_) {
            line1 = pos_.turn() == chess::WHITE ? "White to move" : "Black to move";
            line2 = "bot vs bot";
        } else {
            const bool mine = pos_.turn() == humanSide_;
            line1 = mine ? "Your move" : "Bot is thinking";
            if (pos_.inCheck()) line1 += " - check!";
            line2 = botInfoLine();
            if (pos_.inCheck()) color1 = kCheckMark;
            else if (!mine) color1 = kAccent;
        }
        text(b, line1, {PANEL_X + 12.0f, CARD_STATUS_Y + CARD_STATUS_H - 10.0f}, 1.1f, color1);
        text(b, line2, {PANEL_X + 12.0f, CARD_STATUS_Y + CARD_STATUS_H - 40.0f}, 0.85f, kTextDim);

        // Evaluation bar, always from White's point of view.
        int cp = ai::evaluate(pos_);
        if (pos_.turn() == chess::BLACK) cp = -cp;
        const forge::Rect bar{PANEL_X + 12.0f, CARD_STATUS_Y + 42.0f, PANEL_W - 24.0f - 58.0f,
                              12.0f};
        b.drawRect(bar, kButton);
        const float share = forge::clamp(0.5f + (float)cp / 1600.0f, 0.05f, 0.95f);
        b.drawRect({bar.x, bar.y, bar.w * share, bar.h}, kWhiteBody);
        b.drawRect({bar.center().x - 1.0f, bar.y - 2.0f, 2.0f, bar.h + 4.0f}, kPanelEdge);
        const std::string evalText = scoreText(cp);
        text(b, evalText,
             {PANEL_X + PANEL_W - 12.0f - textW(evalText, 0.8f), CARD_STATUS_Y + 57.0f}, 0.8f,
             kTextDim);

        // Level picker.
        smallText(b, "Level", {PANEL_X + 12.0f, CARD_STATUS_Y + 22.0f}, 0.68f, kTextFaint);
        for (int i = 0; i < 5; ++i) {
            const forge::Rect r = levelPillRect(i);
            const bool on = i + 1 == level_;
            const bool hot = r.contains(mouse_);
            b.drawRect(r, on ? kButtonOn : (hot ? kButtonHot : kButton));
            b.drawRectOutline(r, on ? kAccent : kPanelEdge, 1.0f);
            textCentered(b, std::to_string(i + 1), r.center(), 0.75f, on ? kText : kTextDim);
        }
    }

    void drawMoveList(forge::SpriteBatch& b) const {
        const int plies = (int)records_.size();
        const int totalRows = (plies + 1) / 2;
        const int visible = MOVE_ROWS * 2;
        const int scroll = std::min(moveScroll_, std::max(0, totalRows - visible));
        const int first = std::max(0, totalRows - visible - scroll);
        for (int i = 0; i < visible; ++i) {
            const int row = first + i;
            if (row >= totalRows) break;
            const float x = PANEL_X + 12.0f + (float)(i / MOVE_ROWS) * (PANEL_W * 0.5f - 6.0f);
            const float top =
                CARD_MOVES_Y + CARD_MOVES_H - 14.0f - (float)(i % MOVE_ROWS) * MOVE_ROW_H;
            const std::string number = std::to_string(row + 1) + ".";
            text(b, number, {x + 30.0f - textW(number, 0.78f), top}, 0.78f, kTextFaint);
            if (row * 2 < plies)
                text(b, clip(records_[(size_t)(row * 2)].san), {x + 38.0f, top}, 0.78f, kText);
            if (row * 2 + 1 < plies)
                text(b, clip(records_[(size_t)(row * 2 + 1)].san), {x + 124.0f, top}, 0.78f,
                     kTextDim);
        }
        if (plies == 0)
            text(b, "no moves yet - click a piece", {PANEL_X + 14.0f,
                                                    CARD_MOVES_Y + CARD_MOVES_H - 16.0f},
                 0.8f, kTextFaint);
    }

    void drawCaptured(forge::SpriteBatch& b) const {
        int have[2][7] = {};
        for (int sq = 0; sq < 128; ++sq) {
            if (sq & 0x88) {
                sq += 7;
                continue;
            }
            const int p = pos_.at(sq);
            if (p != chess::EMPTY) ++have[chess::colorOf(p)][chess::typeOf(p)];
        }
        static constexpr int kFull[7] = {0, 8, 2, 2, 2, 1, 0};
        static constexpr int kOrder[5] = {chess::QUEEN, chess::ROOK, chess::BISHOP,
                                          chess::KNIGHT, chess::PAWN};
        // Two rows — Black's trophies on top, White's below — so fifteen pieces
        // per side always fit the card width.
        const float rowTop = CARD_TAKEN_Y + CARD_TAKEN_H - 16.0f;
        for (int side = 0; side < 2; ++side) {
            float x = PANEL_X + 14.0f;
            const float y = side == 0 ? rowTop : rowTop - 26.0f;
            for (int t : kOrder)
                for (int i = 0; i < kFull[t] - have[side][t]; ++i) {
                    drawTrophyPiece(b, t, {x + 11.0f, y}, 22.0f);
                    x += 23.0f;
                }
        }
        // Material balance, right-aligned on the top row (there is always room
        // for it: a full set of trophies ends well before that column).
        int balance = 0;
        for (int t : kOrder)
            balance += (have[chess::WHITE][t] - have[chess::BLACK][t]) * kPieceValue[t];
        if (balance != 0) {
            const std::string s =
                std::string(balance > 0 ? "White +" : "Black +") + std::to_string(std::abs(balance));
            text(b, s, {PANEL_X + PANEL_W - 14.0f - textW(s, 0.75f), rowTop + 9.0f}, 0.75f,
                 kTextDim);
        }
    }

    void drawButtons(forge::SpriteBatch& b) const {
        const char* labels[4] = {"New  N", "Undo  U", "Hint  H", "Flip  F"};
        for (int i = 0; i < 4; ++i) {
            const forge::Rect r = buttonRect(i);
            const bool hot = r.contains(mouse_);
            b.drawRect(r, hot ? kButtonHot : kButton);
            b.drawRectOutline(r, hot ? kAccent : kPanelEdge, 1.0f);
            textCentered(b, labels[i], r.center(), 0.8f, hot ? kText : kTextDim);
        }
    }

    void drawBands(forge::SpriteBatch& b) const {
        bigText(b, "FORGE CHESS", {BOARD_X, FIELD_H - 8.0f}, 0.62f, kText);
        const std::string keys =
            "drag or click to move | arrows + Enter | U undo | F flip | H hint | N new | ESC pause";
        text(b, keys, {FIELD_W - 40.0f - textW(keys, 0.7f), FIELD_H - 14.0f}, 0.7f, kTextFaint);

        std::string left = records_.empty() ? "white to move" : ("last " + records_.back().san);
        if (hintTimer_ > 0.0f && hintMove_ != chess::MOVE_NONE && !hintSan_.empty())
            left = "hint " + hintSan_;
        text(b, left, {PANEL_X, 40.0f}, 0.72f, kTextDim);
        std::string right = std::to_string(plies()) + " plies - " + scoreText(whiteCp());
        text(b, right, {FIELD_W - 40.0f - textW(right, 0.72f), 40.0f}, 0.72f, kTextFaint);
    }

    void drawArrow(forge::SpriteBatch& b, Vec2 from, Vec2 to, forge::Color c) const {
        const Vec2 d = to - from;
        const float len = d.length();
        if (len < 24.0f) return;
        const Vec2 dir = d * (1.0f / len);
        const Vec2 tip = to - dir * 26.0f;
        const Vec2 side{-dir.y, dir.x};
        b.drawLine(from + dir * 16.0f, tip, c, 9.0f);
        b.drawLine(tip, tip - dir * 24.0f + side * 14.0f, c, 9.0f);
        b.drawLine(tip, tip - dir * 24.0f - side * 14.0f, c, 9.0f);
    }

    void drawPromo(forge::SpriteBatch& b) const {
        b.drawRect({BOARD_X, BOARD_Y, BOARD_SIZE, BOARD_SIZE}, {0.0f, 0.0f, 0.0f, 0.5f});
        const forge::Rect back{BOARD_X + BOARD_SIZE * 0.5f - 186.0f,
                               BOARD_Y + BOARD_SIZE * 0.5f - 74.0f, 372.0f, 148.0f};
        b.drawRect(back, kCard);
        b.drawRectOutline(back, kAccentDim, 2.0f);
        textCentered(b, "Promote to", {back.center().x, back.y + back.h - 22.0f}, 0.8f, kTextDim);

        static constexpr int kTypes[4] = {chess::QUEEN, chess::ROOK, chess::BISHOP, chess::KNIGHT};
        static constexpr const char* kKeys[4] = {"Q", "R", "B", "N"};
        for (int i = 0; i < 4; ++i) {
            const forge::Rect r = promoButtonRect(i);
            const bool on = i == promoIndex_;
            const bool hot = r.contains(mouse_);
            b.drawRect(r, on ? kButtonOn : (hot ? kButtonHot : kButton));
            b.drawRectOutline(r, on ? kAccent : kPanelEdge, on ? 3.0f : 1.0f);
            drawPiece(b, kTypes[i], pos_.turn(), {r.center().x, r.center().y + 8.0f}, 70.0f);
            textCentered(b, kKeys[i], {r.center().x, r.y + 15.0f}, 0.7f,
                         on ? kText : kTextDim);
        }
    }

    void drawTitle(forge::SpriteBatch& b) const {
        b.drawRect({0.0f, 0.0f, FIELD_W, FIELD_H}, {0.02f, 0.02f, 0.04f, 0.70f});
        const forge::Vec2 c{BOARD_X + BOARD_SIZE * 0.5f, BOARD_Y + BOARD_SIZE * 0.5f};
        const forge::Rect back{c.x - 320.0f, c.y - 168.0f, 640.0f, 336.0f};
        b.drawRect(back, kCard);
        b.drawRectOutline(back, kAccentDim, 2.0f);

        bigTextCentered(b, "FORGE CHESS", {c.x, c.y + 118.0f}, 1.0f, kText);
        textCentered(b, "the complete rules of chess, ported from amanmprojects/chess",
                     {c.x, c.y + 66.0f}, 0.78f, kTextDim);
        textCentered(b, "castling | en passant | promotion | check & mate | draws",
                     {c.x, c.y + 34.0f}, 0.78f, kTextFaint);
        if (std::fmod(uiTime_, 1.8f) < 1.25f)
            textCentered(b, "press Enter, or click, to play White",
                         {c.x, c.y - 16.0f}, 1.0f, kAccent);
        textCentered(b, std::string("bot: ") + ai::levels()[(size_t)(level_ - 1)].name +
                           " - keys 1-5 change it",
                     {c.x, c.y - 58.0f}, 0.78f, kTextDim);
        textCentered(b, "U undo - F flip - H hint - N new game - ESC pause - Q quit",
                     {c.x, c.y - 100.0f}, 0.72f, kTextFaint);
    }

    void drawPause(forge::SpriteBatch& b) const {
        b.drawRect({0.0f, 0.0f, FIELD_W, FIELD_H}, {0.02f, 0.02f, 0.04f, 0.62f});
        const forge::Vec2 c{BOARD_X + BOARD_SIZE * 0.5f, BOARD_Y + BOARD_SIZE * 0.5f};
        const forge::Rect back{c.x - 220.0f, c.y - 84.0f, 440.0f, 168.0f};
        b.drawRect(back, kCard);
        b.drawRectOutline(back, kAccentDim, 2.0f);
        bigTextCentered(b, "PAUSED", {c.x, c.y + 40.0f}, 0.9f, kText);
        textCentered(b, "ESC or Start resumes", {c.x, c.y - 6.0f}, 0.85f, kTextDim);
        textCentered(b, "N new game - U undo - Q quit", {c.x, c.y - 46.0f}, 0.78f, kTextFaint);
    }

    void drawGameOver(forge::SpriteBatch& b) const {
        const forge::Vec2 c{BOARD_X + BOARD_SIZE * 0.5f, BOARD_Y + BOARD_SIZE * 0.5f};
        b.drawRect({BOARD_X, BOARD_Y, BOARD_SIZE, BOARD_SIZE}, {0.02f, 0.02f, 0.04f, 0.55f});
        const forge::Rect back{c.x - 250.0f, c.y - 92.0f, 500.0f, 184.0f};
        const forge::Color accent =
            winner_ == humanSide_ ? kGood : (winner_ < 0 ? kTextDim : kBad);
        b.drawRect(back, kCard);
        b.drawRectOutline(back, accent, 2.0f);
        bigTextCentered(b, gameOverHeadline(), {c.x, c.y + 46.0f}, 0.9f, accent);
        textCentered(b, resultText(), {c.x, c.y + 2.0f}, 0.85f, kTextDim);
        if (std::fmod(uiTime_, 1.8f) < 1.25f)
            textCentered(b, "Enter - new game - U - undo the last move", {c.x, c.y - 46.0f}, 0.78f,
                         kAccent);
    }

    // ------------------------------------------------------------ geometry
    static forge::Rect buttonRect(int i) {
        const float w = (PANEL_W - 24.0f - 3.0f * 8.0f) / 4.0f;
        return {PANEL_X + 12.0f + (float)i * (w + 8.0f), BTN_Y, w, BTN_H};
    }
    static forge::Rect levelPillRect(int i) {
        const float w = (PANEL_W - 70.0f - 4.0f * 6.0f) / 5.0f;
        return {PANEL_X + 58.0f + (float)i * (w + 6.0f), CARD_STATUS_Y + 10.0f, w, 24.0f};
    }
    static forge::Rect promoButtonRect(int i) {
        const float w = 78.0f, gap = 10.0f;
        const float x0 = BOARD_X + (BOARD_SIZE - (w * 4.0f + gap * 3.0f)) * 0.5f;
        return {x0 + (float)i * (w + gap), BOARD_Y + BOARD_SIZE * 0.5f - 46.0f, w, 92.0f};
    }

    void card(forge::SpriteBatch& b, forge::Rect r) const {
        b.drawRect(r, kPanelBg);
        b.drawRectOutline(r, kPanelEdge, 1.0f);
    }

    void fitCamera(forge::IVec2 vp) {
        cam_.position = {FIELD_W * 0.5f, FIELD_H * 0.5f};
        cam_.zoom = std::min((float)vp.x / FIELD_W, (float)vp.y / FIELD_H);
    }

    // ---------------------------------------------------------------- text
    void text(forge::SpriteBatch& b, const std::string& s, Vec2 topLeft, float scale,
              forge::Color c) const {
        if (font_.valid()) font_.draw(b, s, topLeft, scale, c);
    }
    void textCentered(forge::SpriteBatch& b, const std::string& s, Vec2 center, float scale,
                      forge::Color c) const {
        if (font_.valid()) font_.drawCentered(b, s, center, scale, c);
    }
    float textW(const std::string& s, float scale) const {
        return font_.valid() ? font_.widthOf(s, scale) : 0.0f;
    }
    void bigText(forge::SpriteBatch& b, const std::string& s, Vec2 topLeft, float scale,
                 forge::Color c) const {
        if (fontBig_.valid()) fontBig_.draw(b, s, topLeft, scale, c);
    }
    void bigTextCentered(forge::SpriteBatch& b, const std::string& s, Vec2 center, float scale,
                         forge::Color c) const {
        if (fontBig_.valid()) fontBig_.drawCentered(b, s, center, scale, c);
    }
    void smallText(forge::SpriteBatch& b, const std::string& s, Vec2 topLeft, float scale,
                   forge::Color c) const {
        if (fontSmall_.valid()) fontSmall_.draw(b, s, topLeft, scale, c);
    }
    void smallTextCentered(forge::SpriteBatch& b, const std::string& s, Vec2 center, float scale,
                           forge::Color c) const {
        if (fontSmall_.valid()) fontSmall_.drawCentered(b, s, center, scale, c);
    }

    // --------------------------------------------------------------- misc
    static std::string clip(const std::string& s) {
        return s.size() <= 9 ? s : s.substr(0, 9);
    }
    static std::string nodeCount(uint64_t nodes) {
        if (nodes >= 1000000) return std::to_string(nodes / 1000000) + "M";
        if (nodes >= 1000) return std::to_string(nodes / 1000) + "k";
        return std::to_string(nodes);
    }
    static std::string scoreText(int cp) {
        if (cp >= ai::kMateThreshold) return "mate";
        if (cp <= -ai::kMateThreshold) return "-mate";
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%+.2f", (double)cp / 100.0);
        return buf;
    }
    int whiteCp() const {
        const int cp = ai::evaluate(pos_);
        return pos_.turn() == chess::WHITE ? cp : -cp;
    }
    int plies() const { return (int)records_.size(); }
    void clampScroll() {
        const int rows = ((int)records_.size() + 1) / 2;
        moveScroll_ = std::clamp(moveScroll_, 0, std::max(0, rows - MOVE_ROWS * 2));
    }
    void playSfx(forge::SoundHandle h, float pan = 0.0f) const {
        if (h.valid()) engine_->audio().play(h, 0.85f, pan);
    }

    static bool fileExists(const std::string& path) {
        if (FILE* f = std::fopen(path.c_str(), "rb")) {
            std::fclose(f);
            return true;
        }
        return false;
    }

    static bool loadFont(forge::Font& font, float px, const char* envKey) {
        std::vector<std::string> paths;
        if (const char* env = std::getenv(envKey)) paths.emplace_back(env);
        // Shipped with the example so the web build has text (a browser has no
        // system fonts); the desktop build picks it up too when it is there.
        paths.emplace_back("examples/chess/assets/fonts/DejaVuSansMono-Bold.ttf");
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
        for (const std::string& p : paths)
            if (fileExists(p) && font.loadFile(p, px)) return true;
        return false; // text silently disabled; the game stays playable
    }

    void initAudio() {
        auto& audio = engine_->audio();
        auto add = [&](const std::vector<Note>& notes) {
            const std::vector<uint8_t> wav = synthWav(notes);
            return audio.loadMemory(wav.data(), wav.size());
        };
        sfx_.move = add({{210, 180, 0.035f, 0.35f, 2, 0.0f}});
        sfx_.capture = add({{300, 150, 0.070f, 0.40f, 1, 0.35f},
                            {150, 110, 0.050f, 0.30f, 2, 0.20f}});
        sfx_.castle = add({{170, 140, 0.050f, 0.35f, 2, 0.0f},
                           {240, 200, 0.050f, 0.30f, 2, 0.0f}});
        sfx_.promote = add({{392, 392, 0.060f, 0.30f, 2, 0.0f},
                            {523, 523, 0.060f, 0.30f, 2, 0.0f},
                            {784, 784, 0.100f, 0.30f, 2, 0.0f}});
        sfx_.check = add({{660, 990, 0.070f, 0.32f, 1, 0.0f}});
        sfx_.end = add({{523, 523, 0.100f, 0.32f, 2, 0.0f},
                        {392, 392, 0.100f, 0.32f, 2, 0.0f},
                        {262, 262, 0.240f, 0.34f, 2, 0.0f}});
        sfx_.ui = add({{800, 640, 0.020f, 0.22f, 1, 0.0f}});
    }

    // ------------------------------------------------------------------ pieces
    // Sprites are whole board squares (the web board's 45x45 canvas), so `box`
    // is the square the piece is drawn into: identical framing to
    // amanmprojects/chess, where the piece box is `inset: 0` in its square.
    void drawPiece(forge::SpriteBatch& b, int type, int color, Vec2 center, float box) const {
        if (type == chess::EMPTY) return;
        if (const forge::Texture2D* tex = pieceTex_[color][type]) {
            b.drawTexture(*tex, center, {box, box});
            return;
        }
        drawPieceProcedural(b, type, color, center, box * 0.70f);
    }

    /** Captured-piece rows: the flat `tray` palette, like the web player strip. */
    void drawTrophyPiece(forge::SpriteBatch& b, int type, Vec2 center, float box) const {
        if (type == chess::EMPTY) return;
        const forge::Texture2D* tex = pieceTex_[2][type];
        if (!tex) {
            drawPieceProcedural(b, type, chess::WHITE, center, box * 0.70f);
            return;
        }
        b.drawTexture(*tex, center, {box, box}, 0.0f, {1.0f, 1.0f, 1.0f, 0.82f});
    }

    // Sprites live in examples/chess/assets (wK.png ... tK.png, generated by
    // make_pieces.sh from the SVGs). Probed so ./build/09_chess from the repo
    // root and ./09_chess from the build dir both work; AssetManager
    // hot-reloads them if they change on disk.
    void loadPieceTextures() {
        static const char* files[3][7] = {
            {nullptr, "wP.png", "wN.png", "wB.png", "wR.png", "wQ.png", "wK.png"},
            {nullptr, "bP.png", "bN.png", "bB.png", "bR.png", "bQ.png", "bK.png"},
            {nullptr, "tP.png", "tN.png", "tB.png", "tR.png", "tQ.png", "tK.png"},
        };
        static const char* dirs[] = {
            "examples/chess/assets/", "../examples/chess/assets/",
            "../../examples/chess/assets/",
        };
        for (int c = 0; c < 3; ++c)
            for (int t = chess::PAWN; t <= chess::KING; ++t)
                for (const char* dir : dirs) {
                    const std::string path = std::string(dir) + files[c][t];
                    if (fileExists(path) && (pieceTex_[c][t] = engine_->assets().loadTexture(path)))
                        break;
                }
    }

    // -------------------------------------------------------------- fields
    forge::Engine* engine_ = nullptr;
    forge::Camera2D cam_;
    forge::Font fontBig_, font_, fontSmall_;
    forge::Texture2D* pieceTex_[3][7] = {}; // [palette: w/b/tray][type]
    Vec2 mouse_{};
    int hoverSq_ = -1;

    chess::Position pos_;
    std::vector<PlayedMove> records_;
    chess::Position::Status status_{};
    int winner_ = -1;
    bool gameOver_ = false;
    float gameOverTimer_ = 0.0f;

    Mode mode_ = Mode::Play;
    bool attract_ = false;
    bool syncBot_ = false;
    float botPace_ = 0.0f;  // min. seconds between inline bot moves (web, --record)
    float syncPace_ = 0.0f; // countdown to the next inline bot move
    bool startInTitle_ = false;
    bool paused_ = false;
    bool flip_ = false;
    const int humanSide_ = chess::WHITE;

    chess::MoveList selMoves_{};
    int selSq_ = -1;
    int cursorSq_ = 12; // e2
    chess::Move lastMove_ = chess::MOVE_NONE;
    chess::Move hintMove_ = chess::MOVE_NONE;
    std::string hintSan_;
    float hintTimer_ = 0.0f;
    bool dragging_ = false;
    int dragSq_ = -1;
    Vec2 dragPos_{};
    bool promoOpen_ = false;
    chess::MoveList promoMoves_{};
    int promoIndex_ = 0;
    int moveScroll_ = 0;
    float uiTime_ = 0.0f;

    int level_ = 3;
    uint64_t seed_ = 0x5EEDull;
    AsyncBot bot_;
    ai::Search hintSearch_{size_t(1) << 16};
    BotState botState_ = BotState::Idle;
    Sfx sfx_;
};

// ------------------------------------------------------------------- main
struct Args {
    std::string shot, record, moves, fen, click, press;
    int frames = 0;
    int level = 3;
    int width = 1280, height = 800;
    uint64_t seed = 0x5EED5EEDull;
    bool title = false;
    bool attract = false;
    bool flip = false;
    bool help = false;
};

Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (arg == "--shot") {
            if (const char* v = next()) a.shot = v;
        } else if (arg == "--record") {
            if (const char* v = next()) a.record = v;
        } else if (arg == "--frames") {
            if (const char* v = next()) a.frames = std::atoi(v);
        } else if (arg == "--moves") {
            if (const char* v = next()) a.moves = v;
        } else if (arg == "--fen") {
            if (const char* v = next()) a.fen = v;
        } else if (arg == "--level") {
            if (const char* v = next()) a.level = std::atoi(v);
        } else if (arg == "--seed") {
            if (const char* v = next()) a.seed = std::strtoull(v, nullptr, 0);
        } else if (arg == "--size") {
            if (const char* v = next()) std::sscanf(v, "%dx%d", &a.width, &a.height);
        } else if (arg == "--click") {
            if (const char* v = next()) a.click = v;
        } else if (arg == "--press") {
            if (const char* v = next()) a.press = v;
        } else if (arg == "--title") {
            a.title = true;
        } else if (arg == "--attract") {
            a.attract = true;
        } else if (arg == "--flip") {
            a.flip = true;
        } else if (arg == "--help" || arg == "-h") {
            a.help = true;
        }
    }
    return a;
}

void printUsage() {
    std::printf(
        "09_chess - a complete chess game on Forge\n"
        "\n"
        "  drag or click a piece, then a target square - arrows + Enter also move\n"
        "  U undo - F flip - H hint - N new game - 1-5 bot level - ESC pause - Q quit\n"
        "\n"
        "  --shot FILE   render N frames of bot-vs-bot, save a PNG, exit\n"
        "  --record FILE record N frames of bot-vs-bot to an MP4 (ffmpeg)\n"
        "  --frames N    frames to simulate before capturing/exiting\n"
        "  --moves \"...\"  scripted SAN/UCI opening, e.g. --moves \"e4 e5 Nf3\"\n"
        "  --fen FEN     start from a position instead of the initial one\n"
        "  --level N     bot strength 1..5 (Beginner … Expert)\n"
        "  --size WxH    capture size for --shot/--record (default 1280x800)\n"
        "  --seed N      bot randomness seed\n"
        "  --title       stay on the title screen\n"
        "  --attract     bot vs bot in the window\n"
        "  --flip        start with Black at the bottom\n"
        "  --click LIST  taps: \"e2,e4\" clicks two squares, \"e2>e4\" drags\n"
        "  --press KEY   tap a key last: ESC pause, H hint, Q/R/B/N promotion pick\n"
        "\n"
        "CI=1 ./09_chess        deterministic bot-vs-bot smoke run (exit 0/1)\n");
}

forge::Key parseKeyName(const std::string& name) {
    if (name == "ESC" || name == "Escape") return forge::Key::Escape;
    if (name == "ENTER" || name == "Enter") return forge::Key::Enter;
    if (name == "SPACE" || name == "Space") return forge::Key::Space;
    if (name == "H") return forge::Key::H;
    if (name == "U") return forge::Key::U;
    if (name == "F") return forge::Key::F;
    if (name == "N") return forge::Key::N;
    if (name == "Q") return forge::Key::Q;
    if (name == "R") return forge::Key::R;
    if (name == "B") return forge::Key::B;
    if (name.size() == 1 && name[0] >= '1' && name[0] <= '5')
        return (forge::Key)((int)forge::Key::N1 + (name[0] - '1'));
    return forge::Key::Unknown;
}

bool saveScreenshot(const std::string& path, forge::IVec2 vp) {
    std::vector<uint8_t> px((size_t)vp.x * (size_t)vp.y * 4);
    forge::gl::glReadPixels(0, 0, vp.x, vp.y, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    std::vector<uint8_t> flipped(px.size()); // GL is bottom-up, PNG is top-down
    const size_t row = (size_t)vp.x * 4;
    for (int y = 0; y < vp.y; ++y)
        std::memcpy(&flipped[(size_t)y * row], &px[(size_t)(vp.y - 1 - y) * row], row);
    return stbi_write_png(path.c_str(), vp.x, vp.y, 4, flipped.data(), (int)row) != 0;
}

} // namespace

int main(int argc, char** argv) {
    const Args args = parseArgs(argc, argv);
    if (args.help) {
        printUsage();
        return 0;
    }
    forge::EngineConfig cfg;
    cfg.fixedStep = 1.0f / 60.0f;
    cfg.window.vsync = args.shot.empty() && args.record.empty();
#if FORGE_PLATFORM_WEB
    // The window is the canvas: the field the game is drawn for, scaled by the
    // page's CSS. A non-resizable canvas keeps the drawing buffer stable.
    cfg.window.width = 1280;
    cfg.window.height = 800;
    cfg.window.resizable = false;
#endif
#if FORGE_PLATFORM_WEB
    // Emscripten takes the frame loop over by unwinding main()'s stack, and
    // with wasm exceptions on that unwind runs the destructors of main()'s
    // locals (a unique_ptr owner would free the game under the loop). Statics
    // outlive main and are never destroyed: the web runtime never exits.
    static forge::Engine engine(cfg);
    static ChessGame game;
#else
    forge::Engine engine(cfg);
    ChessGame game;
#endif
    if (!engine.valid()) return 1;
    engine.window().setTitle("Forge 09_chess - rules + bot ported from amanmprojects/chess");

    game.setSeed(args.seed);
    game.setLevel(args.level);
    game.setFlip(args.flip);
    game.setStartInTitle(args.title);
    game.init(engine);
    if (!args.fen.empty() && !game.newGame(args.fen)) {
        std::printf("chess: bad --fen\n");
        return 1;
    }
    if (!args.moves.empty() && !game.playScripted(args.moves)) {
        std::printf("chess: could not play --moves\n");
        return 1;
    }

#if !FORGE_PLATFORM_WEB
    if (!args.record.empty()) {
        // Bot-vs-bot video: inline searches (deterministic) paced to a move
        // every ~half second, one fixed step per frame, rendered offscreen.
        game.setSyncBot(true);
        game.setAttract(true);
        game.setBotPace(0.5f);
        const bool ok = clip::recordClip(
            args.record, args.width, args.height, args.frames > 0 ? args.frames : 1500,
            [&] { game.update(1.0f / 60.0f); },
            [&](forge::Framebuffer* fb) { game.renderTo(fb); }, engine);
        return ok && game.healthy() ? 0 : 1;
    }
#endif

    if (!args.shot.empty()) {
        // Deterministic capture: both sides are the bot, every search runs on
        // this thread with node limits only (no wall clock), so runs repeat.
        const int frames = args.frames > 0 ? args.frames : 900;
        game.setSyncBot(true);
        game.setAttract(args.click.empty() && !args.title);
        for (int i = 0; i < frames; ++i) game.update(1.0f / 60.0f);

        // --click / --press drive the real Input object, exactly like a player:
        // "e2,e4" is click-click, "e2>e4" is a press-drag-release.
        if (!args.click.empty()) {
            std::istringstream list(args.click);
            std::string item;
            while (std::getline(list, item, ',')) {
                const size_t arrow = item.find('>');
                if (arrow != std::string::npos) {
                    const int from = chess::squareFromAlgebraic(item.substr(0, arrow));
                    const int to = chess::squareFromAlgebraic(item.substr(arrow + 1));
                    if (from >= 0 && to >= 0) game.injectDrag(from, to);
                } else {
                    const int sq = chess::squareFromAlgebraic(item);
                    if (sq >= 0) game.injectClick(sq);
                }
            }
        }
        if (!args.press.empty()) game.injectKey(parseKeyName(args.press));

        forge::Framebuffer fbo;
        const bool offscreen = fbo.create(args.width, args.height);
        game.renderTo(offscreen ? &fbo : nullptr);
        const forge::IVec2 shotSize =
            offscreen ? forge::IVec2{args.width, args.height} : engine.viewport();
        if (offscreen) fbo.bind(); // read back the target we just rendered into
        const bool ok = saveScreenshot(args.shot, shotSize);
        if (offscreen) forge::Framebuffer::unbind(engine.viewport().x, engine.viewport().y);
        const bool sane = game.healthy();
        std::printf("shot: %s (%dx%d, %d frames, %d plies) level=%d over=%d %s sane=%d %s\n",
                    args.shot.c_str(), shotSize.x, shotSize.y, frames, game.plyCount(),
                    game.levelIndex(), game.over() ? 1 : 0, game.resultText().c_str(),
                    sane ? 1 : 0, ok ? "OK" : "FAILED");
        return ok && sane ? 0 : 1;
    }

    // CI plays a whole game out by itself; the window shows normal play.
#if FORGE_PLATFORM_WEB
    // The page opens on the title card (the attract game plays behind it) and
    // then hands over to the human. No frame cap: it runs until the tab closes.
    game.setStartInTitle(true);
    game.setAttract(false);
    engine.runFixed([](float dt) { game.update(dt); }, [] { game.render(); });
#else
    const bool ci = std::getenv("CI") != nullptr;
    game.setAttract(args.attract || ci);
    game.setSyncBot(ci);
    const int frameLimit = args.frames > 0 ? args.frames : (ci ? 240 : 0);
    int frames = 0;
    engine.runFixed(
        [&](float dt) { game.update(dt); },
        [&] {
            game.render();
            if (frameLimit > 0 && ++frames >= frameLimit) engine.quit();
        });
#endif
    std::printf("chess: plies=%d level=%d over=%d %s sane=%d\n", game.plyCount(), game.levelIndex(),
                game.over() ? 1 : 0, game.resultText().c_str(), game.healthy() ? 1 : 0);
    return game.healthy() ? 0 : 1;
}
