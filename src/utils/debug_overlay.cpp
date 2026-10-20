#include "forge/utils/debug_overlay.hpp"
#include "forge/graphics/sprite_batch.hpp"
#include "forge/graphics/font.hpp"
#include "forge/core/input.hpp"
#include "forge/scene/scene.hpp"
#include "forge/physics/physics.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace forge {

namespace {
constexpr float kPad = 10.0f;
constexpr float kLine = 18.0f;
}

bool DebugOverlay::init(const Font* font) {
    font_ = font;
    frameMs_.clear();
    return font_ != nullptr;
}

void DebugOverlay::shutdown() {
    font_ = nullptr;
    frameMs_.clear();
}

void DebugOverlay::handleInput(Input& input) {
    if (input.pressed(Key::F1)) {
        opts_.enabled = !opts_.enabled;
        toggled_ = true;
    }
}

void DebugOverlay::update(float dt, const Profiler& profiler, SpriteBatch& batch) {
    (void)profiler;
    if (dt > 0) {
        float ms = dt * 1000.0f;
        frameMsLast_ = ms;
        frameMs_.push_back(ms);
        while (frameMs_.size() > kHistory) frameMs_.pop_front();
        float inst = 1.0f / std::max(dt, 1e-6f);
        fpsSmoothed_ = fpsSmoothed_ > 0 ? fpsSmoothed_ * 0.9f + inst * 0.1f : inst;
    }
    drawCalls_ = batch.drawCalls();
    quads_ = batch.quadsDrawn();
    // Prefer the profiler's zones when the caller wrapped update/render in
    // FORGE_ZONE("update"/"render") (Engine's loops do).
    updateMs_ = (float)profiler.zoneMs("update");
    renderMs_ = (float)profiler.zoneMs("render");
}

float DebugOverlay::averageFps() const {
    if (frameMs_.empty()) return 0.0f;
    double sum = 0;
    for (float ms : frameMs_) sum += ms;
    double avg = sum / frameMs_.size();
    return avg > 0 ? (float)(1000.0 / avg) : 0.0f;
}

float DebugOverlay::onePercentLowMs() const {
    if (frameMs_.empty()) return 0.0f;
    std::vector<float> sorted(frameMs_.begin(), frameMs_.end());
    std::sort(sorted.begin(), sorted.end(), std::greater<float>());
    size_t n = std::max<size_t>(1, sorted.size() / 100);
    return sorted[std::min(n - 1, sorted.size() - 1)];
}

void DebugOverlay::text(SpriteBatch& batch, const std::string& s, Vec2 pos,
                        float scale, Color c) {
    if (font_) font_->draw(batch, s, pos, scale, c);
}

void DebugOverlay::drawGraph(SpriteBatch& batch, Vec2 topRight, Vec2 size, float scale) {
    const float s = scale > 0 ? scale : 1.0f;
    Rect frame{topRight.x - size.x, topRight.y - size.y, size.x, size.y};
    batch.drawRect(frame, Color{0, 0, 0, 0.45f});
    batch.drawRectOutline(frame, Color{1, 1, 1, 0.10f}, 1.0f);

    // Reserve a label strip at the top so the readout never sits on the bars.
    const float labelH = kLine * s;
    Rect bars{frame.x, frame.y, frame.w, frame.h - labelH};
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f ms  %.0f fps", frameMsLast_, fpsSmoothed_);
    text(batch, buf, {frame.x + 6, frame.y + frame.h - kLine * 1.1f * s}, 0.55f * s,
         opts_.text);
    if (frameMs_.empty() || bars.h <= 0) return;

    // The graph tops out at 33.3 ms (30 fps), so a 60 fps frame sits at half
    // height and the 16.7 ms reference line lands in the middle.
    constexpr float kMaxMs = 33.3f;
    float refY = bars.y + bars.h * (1.0f - std::min(1.0f, 16.7f / kMaxMs));
    batch.drawLine({bars.x, refY}, {bars.x + bars.w, refY}, Color{1, 1, 1, 0.18f}, 1.0f);

    float dx = bars.w / (float)kHistory;
    size_t start = kHistory > frameMs_.size() ? kHistory - frameMs_.size() : 0;
    for (size_t i = 0; i < frameMs_.size(); ++i) {
        float ms = frameMs_[i];
        float h = clamp01(ms / kMaxMs) * bars.h;
        float x = bars.x + (float)(start + i) * dx;
        Color c = ms > 20.0f ? opts_.warn : opts_.accent;
        batch.drawRect({x, bars.y, std::max(dx - 1.0f, 1.0f), h}, c);
    }
}

void DebugOverlay::drawPanel(SpriteBatch& batch, Rect r, float scale) {
    (void)scale;
    batch.drawRect(r, opts_.background);
    batch.drawRectOutline(r, Color{1, 1, 1, 0.10f}, 1.0f);
}

void DebugOverlay::draw(SpriteBatch& batch, IVec2 viewport,
                        const Scene* scene, const PhysicsWorld* physics) {
    if (!opts_.enabled || !font_) return;
    const float s = opts_.scale;
    const float vw = (float)viewport.x, vh = (float)viewport.y;
    // Overlay space: center origin, Y-up (matches the default ortho used by
    // most HUD passes). Callers that use a camera matrix should wrap this in a
    // screen-space begin/end.
    const float right = vw * 0.5f, top = vh * 0.5f;

    // Count the lines actually emitted (header + enabled sections) so the
    // panel never clips its own text as options change.
    float lines = 1; // title
    if (opts_.stats) lines += 4;
    if (opts_.scene) lines += 1;
    if (opts_.physics) lines += 1;
    if (opts_.logTail) lines += 3;
    float panelH = lines * kLine * s + kPad * 2;
    float panelW = 330.0f * s;
    Rect panel{right - panelW - kPad, top - panelH - kPad - (opts_.fpsGraph ? 96 * s : 0),
               panelW, panelH};
    drawPanel(batch, panel, s);

    float x = panel.x + kPad;
    float y = panel.y + panel.h - kPad - kLine * s;
    char buf[256];
    auto line = [&](const char* fmt, auto... args) {
        // Call sites pass literals; clang's -Wformat-security cannot tell.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-security"
#endif
        std::snprintf(buf, sizeof(buf), fmt, args...);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
        text(batch, buf, {x, y}, 0.55f * s, opts_.text);
        y -= kLine * s;
    };

    text(batch, "FORGE " FORGE_VERSION "  [F1]", {x, y}, 0.55f * s, opts_.accent);
    y -= kLine * s;
    if (opts_.stats) {
        line("frame %.2f ms   %.0f fps   low1%% %.1f ms",
             frameMsLast_, fpsSmoothed_, onePercentLowMs());
        line("draw calls %d   quads %d", drawCalls_, quads_);
        line("update %.2f ms   render %.2f ms", updateMs_, renderMs_);
        line("frame %zu/%zu", frameMs_.size(), kHistory);
    }
    if (opts_.scene) {
        if (scene) {
            line("entities %zu   physics bodies %zu",
                 scene->registry().entityCount(), scene->physics().bodyCount());
        } else {
            line("entities n/a (pass Scene* to draw)");
        }
    }
    if (opts_.physics) {
        if (physics) {
            line("bodies %zu   joints %zu   ccd %s",
                 physics->bodyCount(), physics->jointCount(),
                 physics->continuous() ? "on" : "off");
        } else {
            line("physics n/a (pass PhysicsWorld* to draw)");
        }
    }
    if (opts_.logTail) {
        const auto& zones = Profiler::instance().zones();
        if (!zones.empty()) {
            const auto& z = zones.back();
            line("zone %s %.2f ms", z.name.c_str(), z.ms);
        }
        line("toggle: F1   screenshot: --shot");
        line("overlay: dependency-free (no ImGui)");
    }
    if (opts_.fpsGraph) {
        drawGraph(batch, {right - kPad, top - kPad}, {300.0f * s, 84.0f * s}, s);
    }
}

} // namespace forge
