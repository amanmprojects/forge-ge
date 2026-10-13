#pragma once
// Engine: owns Window + Input + GL + batch + audio + assets.
// run(update, render): fixed-timestep update, interpolated... (no interp;
// variable render), VSync swap, stats. Subclass or use callbacks.

#include "forge/core/window.hpp"
#include "forge/core/input.hpp"
#include "forge/core/time.hpp"
#include "forge/graphics/sprite_batch.hpp"
#include "forge/graphics/camera.hpp"
#include "forge/assets/assets.hpp"
#include "forge/audio/audio.hpp"
#include <functional>

namespace forge {

struct EngineConfig {
    WindowConfig window;
    float fixedStep = 1.0f / 60.0f; // physics/update step
    int maxSteps = 5;
    bool initAudio = true;
    bool quitOnEscape = false;
    // Frame-rate cap for runFixed/runVariable (0 = uncapped). With vsync on
    // the swap already paces; this is for vsync-off without busy-spinning.
    int maxFps = 0;
};

class Engine {
public:
    explicit Engine(const EngineConfig& cfg = {});
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    bool valid() const { return glReady_; }

    Window& window() { return window_; }
    Input& input() { return input_; }
    Time& time() { return time_; }
    SpriteBatch& batch() { return batch_; }
    AudioEngine& audio() { return audio_; }
    AssetManager& assets() { return assets_; }

    // One-shot: pump events, begin batch. Returns false when quitting.
    // Contract: if this returns false, do NOT call endFrame() (no batch was
    // begun). Profiler/input bookkeeping is left balanced either way.
    bool beginFrame(const Mat4& viewProj);
    void endFrame();

    // Clear the default framebuffer (helper so games don't touch GL directly).
    void clear(float r, float g, float b, float a = 1.0f);

    // Full loop. update(dt) may be called 0..maxSteps times per frame.
    // render(alpha) draws with batch (begin/end handled by you or use beginFrame).
    // Simplest: runFixed(update, render) or runVariable(fn).
    void runFixed(std::function<void(float)> update, std::function<void()> render);
    void runVariable(std::function<void(float)> frame);

    void quit() { quitting_ = true; }
    bool quitting() const { return quitting_; }

    // Viewport helpers
    IVec2 viewport() const;
    Mat4 defaultProj() const; // centered ortho in pixels

    // Stats
    float fps() const { return time_.fps(); }
    int drawCalls() const { return batch_.drawCalls(); }

private:
    bool pumpEvents();
    bool initGL();
    void pace(); // frame limiter (EngineConfig::maxFps)

    Window window_;
    Input input_;
    Time time_;
    SpriteBatch batch_;
    AudioEngine audio_;
    AssetManager assets_{&audio_};
    EngineConfig cfg_;
    FixedStepper stepper_;
    bool glReady_ = false;
    bool quitting_ = false;
    void* gamepads_[4] = {};
    uint64_t lastSwap_ = 0;
};

} // namespace forge
