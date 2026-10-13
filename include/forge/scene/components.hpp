#pragma once
// Built-in components: Transform, Sprite, Velocity, Collider, Tag, Script hooks.

#include "forge/math/math.hpp"
#include "forge/graphics/texture.hpp"
#include <string>
#include <functional>

namespace forge {

class Registry;
using Entity = uint32_t;

struct Transform {
    Vec2 pos{0, 0};
    float rotation = 0.0f;
    Vec2 scale{1, 1};
};

struct Sprite {
    const Texture2D* texture = nullptr;
    Rect src{0, 0, 0, 0};   // pixel rect; empty = full texture
    Color color = Color::white();
    int layer = 0;          // draw order
    bool visible = true;
    bool flipX = false, flipY = false;
    Vec2 size{0, 0};        // 0 = use texture size
};

struct Velocity {
    Vec2 v{0, 0};
    Vec2 accel{0, 0};
    float damping = 0.0f;
    float maxSpeed = 0.0f; // 0 = unlimited
};

struct Collider {
    Vec2 halfExtents{16, 16}; // box around transform
    bool solid = true;
    bool sensor = false;
};

struct Tag {
    std::string name;
};

// Lightweight per-entity script: called by Scene each frame.
struct Script {
    std::function<void(Entity, Registry&, float)> onUpdate;
    std::function<void(Entity, Registry&)> onStart;
    bool started = false;
};

} // namespace forge
