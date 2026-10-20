#include "forge/graphics/postprocess.hpp"
#include "forge/graphics/framebuffer.hpp"
#include "forge/graphics/gl_loader.hpp"

namespace forge {

const char* FullscreenPass::kFullscreenVert = R"GLSL(
#version 330 core
layout(location = 0) in vec2 aPos;
out vec2 vUV;
void main() {
    vUV = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)GLSL";

const char* FullscreenPass::kVignetteFrag = R"GLSL(
#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTexture;
uniform vec2 uResolution;
uniform float uTime;
uniform float uVignette;
uniform float uVignetteRadius;
uniform float uScanlines;
uniform int uFlipY;

void main() {
    vec2 uv = uFlipY != 0 ? vec2(vUV.x, 1.0 - vUV.y) : vUV;
    vec4 c = texture(uTexture, uv);
    // Vignette: radial falloff from the center.
    vec2 d = vUV - vec2(0.5);
    d.x *= uResolution.x / max(uResolution.y, 1.0);
    float dist = length(d) * 1.41421356;
    float vig = smoothstep(uVignetteRadius, 1.0, dist);
    c.rgb *= 1.0 - vig * uVignette;
    // Scanlines: subtle horizontal darkening.
    float line = 0.5 + 0.5 * sin(vUV.y * uResolution.y * 3.14159265);
    c.rgb *= 1.0 - uScanlines * line;
    FragColor = c;
}
)GLSL";

namespace {

struct FullscreenQuad {
    uint32_t vao = 0, vbo = 0;

    bool init() {
        if (vao) return true;
        if (!gl::isLoaded()) return false;
        gl::glGenVertexArrays(1, &vao);
        gl::glBindVertexArray(vao);
        gl::glGenBuffers(1, &vbo);
        gl::glBindBuffer(GL_ARRAY_BUFFER, vbo);
        const float verts[12] = {
            -1, -1,
             1, -1,
             1,  1,
            -1,  1,
        };
        gl::glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
        gl::glEnableVertexAttribArray(0);
        gl::glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
        gl::glBindVertexArray(0);
        return true;
    }

    void draw() {
        gl::glBindVertexArray(vao);
        gl::glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        gl::glBindVertexArray(0);
    }
};

FullscreenQuad& quad() {
    static FullscreenQuad q;
    q.init();
    return q;
}

} // namespace

bool FullscreenPass::load(const char* vertSrc, const char* fragSrc) {
    return shader_.load(vertSrc, fragSrc);
}

bool FullscreenPass::loadFiles(const std::string& vertPath, const std::string& fragPath) {
    return shader_.loadFiles(vertPath, fragPath);
}

void FullscreenPass::apply(const Texture2D& src, IVec2 viewport, bool flipY) {
    if (!valid() || !src.valid() || viewport.x <= 0 || viewport.y <= 0) return;
    FullscreenQuad& q = quad();
    if (!q.vao) return;

    shader_.bind();
    shader_.setInt("uTexture", 0);
    shader_.setVec2("uResolution", {(float)viewport.x, (float)viewport.y});
    shader_.setInt("uFlipY", flipY ? 1 : 0);
    src.bind(0);

    gl::glDisable(GL_BLEND);
    gl::glDisable(GL_SCISSOR_TEST);
    gl::glViewport(0, 0, viewport.x, viewport.y);
    q.draw();
}

bool PostProcess::init(Effect effect) {
    effect_ = effect;
    switch (effect) {
        case Effect::Vignette:
        case Effect::Scanlines:
            return pass_.load(FullscreenPass::kFullscreenVert, FullscreenPass::kVignetteFrag);
        case Effect::Custom:
            break;
    }
    return false;
}

void PostProcess::setVignette(float strength, float radius) {
    vignetteStrength_ = clamp01(strength);
    vignetteRadius_ = clamp01(radius);
}

void PostProcess::setScanlines(float intensity) { scanlineIntensity_ = clamp01(intensity); }

void PostProcess::apply(const Texture2D& src, IVec2 viewport) {
    if (!valid()) return;
    pass_.setFloat("uVignette", vignetteStrength_);
    pass_.setFloat("uVignetteRadius", vignetteRadius_);
    pass_.setFloat("uScanlines", scanlineIntensity_);
    pass_.setFloat("uTime", time_);
    pass_.apply(src, viewport, false);
}

// ------------------------------------------------------- RenderTargetStack
RenderTargetStack::~RenderTargetStack() { clear(); }

void RenderTargetStack::clear() {
    for (auto& e : stack_) { delete e.view; delete e.fbo; }
    stack_.clear();
    for (auto& p : pool_) { delete p.view; delete p.fbo; }
    pool_.clear();
}

const Texture2D& RenderTargetStack::push(int width, int height) {
    if (width <= 0) width = 1;
    if (height <= 0) height = 1;
    Pooled* use = nullptr;
    for (auto& p : pool_) {
        if (p.w == width && p.h == height) { use = &p; break; }
    }
    if (!use) {
        Pooled p;
        p.fbo = new Framebuffer();
        if (!p.fbo->create(width, height)) { delete p.fbo; return Texture2D::white(); }
        p.view = new Texture2D();
        p.w = width; p.h = height;
        pool_.push_back(p);
        use = &pool_.back();
    }
    Entry e;
    e.fbo = use->fbo;
    e.view = use->view;
    e.w = width; e.h = height;
    stack_.push_back(e);
    e.fbo->bind();
    return *current();
}

const Texture2D* RenderTargetStack::pop(int screenW, int screenH) {
    if (stack_.empty()) return nullptr;
    stack_.pop_back();
    if (stack_.empty()) {
        Framebuffer::unbind(screenW, screenH);
        return nullptr;
    }
    Entry& top = stack_.back();
    top.fbo->bind();
    return top.view;
}

const Texture2D* RenderTargetStack::current() const {
    if (stack_.empty()) return nullptr;
    Entry& top = const_cast<Entry&>(stack_.back());
    // Keep the non-owning view in sync with the FBO's (possibly recreated) id.
    top.view->adoptRaw(top.fbo->texture(), top.fbo->width(), top.fbo->height(), false);
    return top.view;
}

} // namespace forge
