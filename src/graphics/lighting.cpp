#include "forge/graphics/lighting.hpp"
#include "forge/graphics/sprite_batch.hpp"
#include "forge/graphics/camera.hpp"
#include "forge/graphics/texture.hpp"
#include "forge/graphics/gl_loader.hpp"
#include <cmath>
#include <algorithm>

namespace forge {

namespace {
constexpr int kRadialSize = 128;
}

bool LightingSystem::init(int width, int height) {
    bool ok = lightmap_.create(std::max(1, width), std::max(1, height));
    if (ok && !lightmapView_) lightmapView_ = std::make_unique<Texture2D>();
    return ok;
}

void LightingSystem::shutdown() {
    lightmapView_.reset();
    lightmap_.destroy();
}

void LightingSystem::resize(int width, int height) {
    lightmap_.resize(std::max(1, width), std::max(1, height));
}

int LightingSystem::addLight(const Light2D& l) {
    lights_.push_back(l);
    return (int)lights_.size() - 1;
}

Light2D* LightingSystem::light(int index) {
    if (index < 0 || index >= (int)lights_.size()) return nullptr;
    return &lights_[(size_t)index];
}

void LightingSystem::update(float dt) {
    // Flicker is evaluated in drawLights() from this clock. Keeping it out of
    // `Light2D::intensity` means callers can re-add lights every frame (as
    // 10_lantern does) without the modulation compounding.
    time_ += dt;
}

const Texture2D& LightingSystem::lightmapTexture() const {
    // Non-owning view over the Framebuffer's color texture (re-wrapped each
    // call: a resize recreates the id, and the view must follow it).
    if (!lightmapView_) lightmapView_ = std::make_unique<Texture2D>();
    lightmapView_->adoptRaw(lightmap_.texture(), lightmap_.width(), lightmap_.height(), false);
    return *lightmapView_;
}

Texture2D& LightingSystem::radialTexture() {
    // White center fading to transparent black at the edge (smoothstep-ish).
    static Texture2D* tex = nullptr;
    if (tex && tex->valid()) return *tex;
    static Texture2D fallback;
    if (!gl::isLoaded()) return fallback;
    std::vector<uint8_t> px((size_t)kRadialSize * kRadialSize * 4, 0);
    const float c = (kRadialSize - 1) * 0.5f;
    for (int y = 0; y < kRadialSize; ++y)
        for (int x = 0; x < kRadialSize; ++x) {
            float dx = (x - c) / c, dy = (y - c) / c;
            float d = std::sqrt(dx * dx + dy * dy);
            float a = clamp01(1.0f - d);
            a = a * a * (3.0f - 2.0f * a); // smoothstep
            size_t i = ((size_t)y * kRadialSize + x) * 4;
            px[i + 0] = 255; px[i + 1] = 255; px[i + 2] = 255;
            px[i + 3] = (uint8_t)(a * 255.0f);
        }
    if (!tex) tex = new Texture2D(); // leaked singleton, same reason as white()
    tex->loadMemory(px.data(), kRadialSize, kRadialSize, 4,
                    TextureFilter::Linear, TextureWrap::Clamp);
    return *tex;
}

void LightingSystem::drawShadows(SpriteBatch& batch, const Light2D& l) {
    // Shadow volume: extrude each light-facing occluder edge away from the
    // light and fill the resulting quad. Approximation (no umbra merging) but
    // correct for the platformer geometry this is meant for.
    const float far = l.radius * 4.0f;
    for (auto& s : occluders_) {
        Vec2 mid = (s.a + s.b) * 0.5f;
        Vec2 toLight = l.pos - mid;
        if (toLight.lengthSq() > (l.radius + far) * (l.radius + far)) continue;
        Vec2 edge = s.b - s.a;
        if (edge.lengthSq() < EPSILON) continue;
        Vec2 n{edge.y, -edge.x};
        // Skip back-facing edges (normal pointing away from the light).
        if (n.dot(toLight) <= 0) continue;
        Vec2 a2 = s.a + (s.a - l.pos).normalized() * far;
        Vec2 b2 = s.b + (s.b - l.pos).normalized() * far;
        // Quad (a, b, b2, a2) is CCW when the edge faces the light.
        batch.drawQuad(s.a, s.b, b2, a2, Color::black());
    }
}

void LightingSystem::drawLights(SpriteBatch& batch, const Camera2D& cam, IVec2 vp) {
    // Camera view rect in world space (for culling).
    Vec2 half{(float)vp.x * 0.5f / std::max(cam.zoom, 0.001f),
              (float)vp.y * 0.5f / std::max(cam.zoom, 0.001f)};
    Rect view{cam.position.x - half.x, cam.position.y - half.y, half.x * 2, half.y * 2};

    Texture2D& radial = radialTexture();
    batch.setBlendMode(SpriteBatch::BlendMode::Additive);
    for (auto& l : lights_) {
        // Deterministic, phase-offset sine flicker (no RNG: replays match).
        float intensity = l.intensity;
        if (l.flickerAmount > 0)
            intensity *= 1.0f + std::sin((time_ + l.flickerPhase) * l.flickerSpeed) *
                                     l.flickerAmount;
        if (!l.enabled || l.radius <= 0 || intensity <= 0) continue;
        Rect bounds = Rect::fromCenter(l.pos, {l.radius * 2, l.radius * 2});
        if (!bounds.overlaps(view)) continue;
        Color c = l.color;
        c.a = clamp01(c.a * intensity);
        batch.drawTexture(radial, l.pos, {l.radius * 2, l.radius * 2}, 0, c);
        if (l.shadows && !occluders_.empty()) {
            // Shadow volumes are painted black with blending off: they cut the
            // lightmap, including the ambient fill.
            batch.setBlendMode(SpriteBatch::BlendMode::None);
            drawShadows(batch, l);
            batch.setBlendMode(SpriteBatch::BlendMode::Additive);
        }
    }
    batch.setBlendMode(SpriteBatch::BlendMode::Alpha);
}

void LightingSystem::render(SpriteBatch& batch, const Camera2D& cam, IVec2 viewport,
                            Framebuffer* target) {
    if (!lightmap_.valid()) return;
    IVec2 vp = viewport;
    if (vp.x <= 0 || vp.y <= 0) return;
    if (lightmap_.width() != vp.x || lightmap_.height() != vp.y) resize(vp.x, vp.y);

    // 1) Lightmap: ambient fill + additive lights (+ black shadow volumes).
    lightmap_.bind();
    gl::glClearColor(ambient_.r, ambient_.g, ambient_.b, 1.0f);
    gl::glClear(GL_COLOR_BUFFER_BIT);
    batch.begin(cam.viewProj(vp.x, vp.y));
    drawLights(batch, cam, vp);
    batch.end();

    // 2) Composite: multiply the lightmap over the scene.
    if (target) target->bind();
    else Framebuffer::unbind(vp.x, vp.y);
    batch.begin(Mat4::ortho(-vp.x * 0.5f, vp.x * 0.5f, -vp.y * 0.5f, vp.y * 0.5f));
    batch.setBlendMode(SpriteBatch::BlendMode::Multiply);
    // Both the lightmap and the target share the GL bottom-left origin, and
    // SpriteBatch's UVs put v0 on the bottom edge, so no V flip is needed.
    const Texture2D& lm = lightmapTexture();
    SpriteDraw d;
    d.tex = &lm;
    d.pos = {0, 0};
    d.size = {(float)vp.x, (float)vp.y};
    d.src = {0, 0, 1, 1};
    d.color = Color::white();
    batch.draw(d);
    batch.setBlendMode(SpriteBatch::BlendMode::Alpha);
    batch.end();
}

} // namespace forge
