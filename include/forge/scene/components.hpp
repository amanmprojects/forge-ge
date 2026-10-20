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

// Physics box around the entity's Transform. Scene::update syncs every
// entity that has BOTH Transform and Collider into its PhysicsWorld: entities
// that also have a Velocity become dynamic bodies, the rest are static.
// After the step, non-static bodies are written back to Transform/Velocity.
// Use scene.physics() directly for kinematic control, gravity tuning, or
// bodies outside the ECS.
struct Collider {
    Vec2 halfExtents{16, 16}; // box around transform (full size = 2*halfExtents)
    bool solid = true;        // false: overlaps are reported, but no pushback
    bool sensor = false;      // report contacts without resolving
    bool kinematic = false;   // script-moved platform: pushes dynamics, not pushed
    float friction = 0.0f;    // ground friction when resting on it
    float gravityScale = 1.0f;
};

struct Tag {
    std::string name;
};

// Data-only script id. Serialized with the scene so the game can re-bind the
// behavior by name after loading (`ScriptRef{"player"}` -> `Script` closure).
struct ScriptRef {
    std::string name;
};

// Lightweight per-entity script: called by Scene each frame.
struct Script {
    std::function<void(Entity, Registry&, float)> onUpdate;
    std::function<void(Entity, Registry&)> onStart;
    bool started = false;
};

} // namespace forge
