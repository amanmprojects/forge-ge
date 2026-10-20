#pragma once
// DebugOverlay: dependency-free developer overlay (F1).
//
// Draws an FPS graph, frame-time breakdown, draw-call/quad counts, entity and
// physics counts, the log tail, and a small text panel. No ImGui, no extra
// dependency, no separate renderer: it uses the same SpriteBatch + Font as the
// game, so it works everywhere Forge works (including the web build).
//
//   DebugOverlay overlay;
//   overlay.init(&font);
//   // after the world + HUD are drawn:
//   overlay.update(dt, engine.profiler(), batch, engine.viewport());
//   overlay.draw(batch, engine.viewport(), scene, &physics);
//
// Toggle with F1 (handled internally when `keyboard` is true).

#include "forge/math/math.hpp"
#include "forge/utils/profiler.hpp"
#include <string>
#include <deque>
#include <cstdint>

namespace forge {

class SpriteBatch;
class Font;
class Input;
class Scene;
class PhysicsWorld;

struct DebugOverlayOptions {
    bool enabled = true;
    bool fpsGraph = true;
    bool stats = true;
    bool logTail = true;
    bool physics = true;
    bool scene = true;
    float scale = 1.0f;
    Color background{0.04f, 0.05f, 0.08f, 0.86f};
    Color text = Color::fromHex(0xD8E1F0FF);
    Color accent = Color::fromHex(0x4FC3F7FF);
    Color warn = Color::fromHex(0xFFB74DFF);
};

class DebugOverlay {
public:
    DebugOverlay() = default;

    bool init(const Font* font);
    void shutdown();
    bool valid() const { return font_ != nullptr; }

    // F1 toggles when `input` is provided.
    void handleInput(Input& input);

    // Feed the per-frame timing + counts. Call once per frame.
    void update(float dt, const Profiler& profiler, SpriteBatch& batch);
    // Draw the overlay. `scene`/`physics` are optional (nullptr = omit).
    void draw(SpriteBatch& batch, IVec2 viewport, const Scene* scene = nullptr,
              const PhysicsWorld* physics = nullptr);

    void setOptions(const DebugOverlayOptions& o) { opts_ = o; }
    DebugOverlayOptions& options() { return opts_; }
    bool enabled() const { return opts_.enabled; }
    void setEnabled(bool on) { opts_.enabled = on; }

    // Average FPS over the graph window (for tests/HUDs).
    float averageFps() const;
    // 1% low frame time in milliseconds (worst 1% over the window).
    float onePercentLowMs() const;

private:
    void drawGraph(SpriteBatch& batch, Vec2 topRight, Vec2 size, float scale);
    void drawPanel(SpriteBatch& batch, Rect r, float scale);
    void text(SpriteBatch& batch, const std::string& s, Vec2 pos, float scale, Color c);

    const Font* font_ = nullptr;
    DebugOverlayOptions opts_;
    std::deque<float> frameMs_;
    static constexpr size_t kHistory = 240;
    float fpsSmoothed_ = 0.0f;
    // Cache of the last frame's counters (the batch resets them per frame).
    int drawCalls_ = 0;
    int quads_ = 0;
    float frameMsLast_ = 0.0f;
    float updateMs_ = 0.0f;
    float renderMs_ = 0.0f;
    bool toggled_ = false;
};

} // namespace forge
