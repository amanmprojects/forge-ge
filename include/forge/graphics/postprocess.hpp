#pragma once
// PostProcess / FullscreenPass: a shader + source texture drawn over the whole
// viewport, plus a small render-target stack so games can bounce between
// offscreen targets without leaking GL state.
//
//   PostProcess fx;
//   fx.init(PostProcess::Effect::Vignette);
//   // draw your scene into a Framebuffer, then:
//   Framebuffer::unbind(vp.x, vp.y);
//   fx.apply(fbo.texture(), vp);
//
// All apply() calls must happen BETWEEN SpriteBatch passes (they change the
// bound program and VAO). Built-in effects: vignette + scanlines.

#include "forge/math/math.hpp"
#include "forge/graphics/shader.hpp"
#include "forge/graphics/texture.hpp"
#include <string>
#include <vector>

namespace forge {

class SpriteBatch;
class Framebuffer;

class FullscreenPass {
public:
    FullscreenPass() = default;

    bool load(const char* vertSrc, const char* fragSrc);
    bool loadFiles(const std::string& vertPath, const std::string& fragPath);
    bool valid() const { return shader_.valid(); }
    void destroy() { shader_.destroy(); }

    // Uniform helpers (locations are cached by Shader).
    void setFloat(const char* name, float v) { shader_.setFloat(name, v); }
    void setVec2(const char* name, Vec2 v) { shader_.setVec2(name, v); }
    void setVec4(const char* name, float r, float g, float b, float a) {
        shader_.setVec4(name, r, g, b, a);
    }
    void setInt(const char* name, int v) { shader_.setInt(name, v); }

    // Draws `src` over the whole viewport. Sets uTexture/uResolution.
    // Framebuffer textures are already bottom-left origin like the target, so
    // `flipY` defaults to false; pass true only for top-down image sources.
    void apply(const Texture2D& src, IVec2 viewport, bool flipY = false);

    // Source is bound as uTexture; the pass also gets uResolution/uProj.
    static const char* kFullscreenVert;
    // Vignette + scanlines driven by uVignette/uVignetteRadius/uScanlines.
    static const char* kVignetteFrag;

private:
    Shader shader_;
};

// Built-in post effects built on FullscreenPass.
class PostProcess {
public:
    enum class Effect { Vignette, Scanlines, Custom };

    bool init(Effect effect = Effect::Vignette);
    void shutdown() { pass_.destroy(); }
    bool valid() const { return pass_.valid(); }

    // Vignette strength 0..1, radius 0..1; scanline intensity 0..1.
    void setVignette(float strength, float radius = 0.75f);
    void setScanlines(float intensity);
    void setTime(float t) { time_ = t; }

    // Renders `src` full-screen with the effect. `viewport` is in pixels.
    void apply(const Texture2D& src, IVec2 viewport);
    FullscreenPass& pass() { return pass_; }

private:
    FullscreenPass pass_;
    Effect effect_ = Effect::Vignette;
    float vignetteStrength_ = 0.55f;
    float vignetteRadius_ = 0.75f;
    float scanlineIntensity_ = 0.25f;
    float time_ = 0.0f;
};

// Push/pop offscreen targets. push() binds a framebuffer (creating/resizing it
// on first use) and returns its texture; pop() restores the previous target.
class RenderTargetStack {
public:
    RenderTargetStack() = default;
    ~RenderTargetStack();

    // Push a new target of `viewport` size. Reuses pooled framebuffers.
    const Texture2D& push(int width, int height);
    // Pop the top target and restore the previous one (or the screen when the
    // stack becomes empty). Returns the popped texture, or nullptr.
    const Texture2D* pop(int screenW, int screenH);
    // The target currently bound, or nullptr for the screen.
    const Texture2D* current() const;
    size_t depth() const { return stack_.size(); }
    void clear();

private:
    struct Entry {
        Framebuffer* fbo = nullptr;
        Texture2D* view = nullptr; // non-owning view of fbo->texture()
        int w = 0, h = 0;
    };
    struct Pooled {
        Framebuffer* fbo = nullptr;
        Texture2D* view = nullptr;
        int w = 0, h = 0;
    };
    std::vector<Entry> stack_;
    std::vector<Pooled> pool_;
};

} // namespace forge
