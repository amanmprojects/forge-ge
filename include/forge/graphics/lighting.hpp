#pragma once
// LightingSystem: 2D lightmap compositing without normal maps.
//
// How it works: lights are rendered additively into an offscreen lightmap
// (starting from the ambient color), then the lightmap is multiplied over the
// already-drawn scene. White = unchanged, black = fully dark. That gives
// colored ambient light, colored point lights, flicker and soft falloff with
// nothing but the sprite batch.
//
//   lighting.setAmbient(Color::fromHex(0x202030FF));
//   lighting.addLight({.pos = {100, 50}, .radius = 220, .color = warm});
//   // per frame, after drawing the world:
//   lighting.render(batch, cam, engine.viewport());
//
// Shadows: add occluder segments with `addOccluder` and set
// `Light2D::shadows = true`. Shadow volumes are drawn as black polygons into
// the lightmap after the lights, so they also darken the ambient term.

#include "forge/math/math.hpp"
#include "forge/graphics/framebuffer.hpp"
#include "forge/graphics/texture.hpp"
#include <vector>
#include <memory>
#include <cstdint>

namespace forge {

class SpriteBatch;
class Camera2D;

struct Light2D {
    Vec2 pos{0, 0};
    float radius = 200.0f;
    Color color = Color::white();
    float intensity = 1.0f;
    // Flicker: the rendered intensity is intensity * (1 + sin(t*speed+phase)*amount).
    // `intensity` stays the authored value; the modulation is computed at draw
    // time, so it never compounds across frames and re-adding lights is safe.
    float flickerAmount = 0.0f;
    float flickerSpeed = 0.0f;
    float flickerPhase = 0.0f;
    bool shadows = false;
    bool enabled = true;
};

struct OccluderSegment {
    Vec2 a, b;
};

class LightingSystem {
public:
    LightingSystem() = default;

    bool init(int width, int height);
    void shutdown();
    bool valid() const { return lightmap_.valid(); }
    void resize(int width, int height);
    int width() const { return lightmap_.width(); }
    int height() const { return lightmap_.height(); }

    void setAmbient(Color c) { ambient_ = c; }
    Color ambient() const { return ambient_; }

    int addLight(const Light2D& l);
    void clearLights() { lights_.clear(); }
    Light2D* light(int index);
    size_t lightCount() const { return lights_.size(); }

    void addOccluder(Vec2 a, Vec2 b) { occluders_.push_back({a, b}); }
    void clearOccluders() { occluders_.clear(); }
    size_t occluderCount() const { return occluders_.size(); }

    void update(float dt);

    // Renders the lightmap and multiplies it over the current framebuffer.
    // Call after the scene is drawn. The batch may be mid-batch or between
    // batches; this function begins and ends its own passes.
    //
    // `target` selects where the composite lands: nullptr = the default
    // framebuffer (screen), otherwise the given offscreen target is re-bound
    // after the lightmap pass (needed when the scene itself is rendered
    // offscreen, e.g. before a post-process pass).
    void render(SpriteBatch& batch, const Camera2D& cam, IVec2 viewport,
                Framebuffer* target = nullptr);

    // The raw lightmap texture (for custom compositing).
    const Texture2D& lightmapTexture() const;

private:
    void drawLights(SpriteBatch& batch, const Camera2D& cam, IVec2 vp);
    void drawShadows(SpriteBatch& batch, const Light2D& l);
    static Texture2D& radialTexture();

    Framebuffer lightmap_;
    // Non-owning view of lightmap_'s color texture for SpriteBatch draws.
    mutable std::unique_ptr<Texture2D> lightmapView_;
    Color ambient_{0.08f, 0.08f, 0.12f, 1.0f};
    std::vector<Light2D> lights_;
    std::vector<OccluderSegment> occluders_;
    float time_ = 0.0f;
};

} // namespace forge
