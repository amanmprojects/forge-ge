#include "forge/graphics/camera.hpp"

namespace forge {

Mat4 Camera2D::viewProj(int vpW, int vpH) const {
    Vec2 shake = shakeOffset();
    Vec2 pos = position + shake;

    // view: translate(-pos) * rotate(-rot) * scale(zoom)
    Mat4 view = Mat4::translate({-pos.x, -pos.y})
              * Mat4::rotate(-rotation)
              * Mat4::scale({zoom, zoom});
    // proj: ortho centered on screen
    float hw = vpW * 0.5f, hh = vpH * 0.5f;
    Mat4 proj = Mat4::ortho(-hw, hw, -hh, hh);
    return proj * view;
}

Vec2 Camera2D::screenToWorld(Vec2 screenPx, int vpW, int vpH) const {
    // screenPx: top-left origin (SDL style). World: center-camera.
    float nx = screenPx.x - vpW * 0.5f;
    float ny = vpH * 0.5f - screenPx.y; // flip Y
    Vec2 p{nx / zoom, ny / zoom};
    p = p.rotated(rotation);
    return p + position + shakeOffset();
}

Vec2 Camera2D::worldToScreen(Vec2 world, int vpW, int vpH) const {
    Vec2 d = world - position - shakeOffset();
    d = d.rotated(-rotation);
    d *= zoom;
    return {d.x + vpW * 0.5f, vpH * 0.5f - d.y};
}

void Camera2D::addTrauma(float amount) {
    trauma_ = clamp01(trauma_ + amount);
}

void Camera2D::update(float dt) {
    time_ += dt;
    trauma_ = trauma_ - traumaDecay_ * dt;
    if (trauma_ < 0) trauma_ = 0;
}

Vec2 Camera2D::shakeOffset() const {
    if (trauma_ <= 0) return {0, 0};
    float s = trauma_ * trauma_ * maxShake_;
    // deterministic noise so shake is smooth, not jitter
    float t = time_ * 60.0f;
    return {
        s * (valueNoise2D(t, 0.0f) * 2.0f - 1.0f),
        s * (valueNoise2D(0.0f, t) * 2.0f - 1.0f)
    };
}

void Camera2D::follow(Vec2 target, float smoothing, float dt) {
    position = position.lerpTo(target, 1.0f - std::exp(-smoothing * dt));
}

} // namespace forge
