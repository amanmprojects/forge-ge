#pragma once
// Camera2D: position / zoom / rotation + screen shake (trauma) + follow.
// Produces a view-projection matrix for SpriteBatch.

#include "forge/math/math.hpp"

namespace forge {

class Camera2D {
public:
    Vec2 position{0, 0};
    float zoom = 1.0f;      // >1 zooms in
    float rotation = 0.0f;  // radians, CCW

    Camera2D() = default;
    Camera2D(Vec2 pos, float z = 1.0f) : position(pos), zoom(z) {}

    // viewport in pixels (drawable size). Origin bottom-left in world->clip.
    Mat4 viewProj(int vpW, int vpH) const;

    Vec2 screenToWorld(Vec2 screenPx, int vpW, int vpH) const;
    Vec2 worldToScreen(Vec2 world, int vpW, int vpH) const;

    // --- screen shake ---
    void addTrauma(float amount);       // 0..1, accumulates
    void update(float dt);              // decays trauma, advances noise time
    Vec2 shakeOffset() const;           // current frame offset in world units
    float trauma() const { return trauma_; }
    void setTraumaDecay(float d) { traumaDecay_ = d; }
    void setMaxShake(float px) { maxShake_ = px; }

    // --- follow ---
    void follow(Vec2 target, float smoothing, float dt); // exp smoothing

private:
    float trauma_ = 0.0f;
    float traumaDecay_ = 1.6f;
    float maxShake_ = 24.0f;
    float time_ = 0.0f;
};

} // namespace forge
