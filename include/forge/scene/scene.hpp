#pragma once
// Scene: owns a Registry, updates scripts/velocities, draws sprites sorted
// by layer. Systems are plain functions so games can add their own.

#include "forge/scene/ecs.hpp"
#include "forge/scene/components.hpp"
#include "forge/physics/physics.hpp"
#include <unordered_map>

namespace forge {

class SpriteBatch;
class Camera2D;

class Scene {
public:
    Scene() = default;

    Registry& registry() { return reg_; }
    const Registry& registry() const { return reg_; }

    // Physics world used by the built-in sync (Transform+Collider entities).
    // Bodies added here manually are never touched by the sync.
    PhysicsWorld& physics() { return physics_; }
    const PhysicsWorld& physics() const { return physics_; }

    Entity create(const std::string& tag = "");
    void destroy(Entity e);
    void clear();

    // Built-in pipeline
    void update(float dt);
    void draw(SpriteBatch& batch, const Camera2D* cam = nullptr);

    // Helpers
    Entity findByTag(const std::string& tag);
    void each(const std::function<void(Entity)>& fn);

private:
    void syncPhysics();     // create/update/retract bodies for collider entities
    void writebackPhysics(); // body pos/vel -> Transform/Velocity (non-static)

    Registry reg_;
    PhysicsWorld physics_;
    std::unordered_map<Entity, uint32_t> bodyIds_; // entity -> physics body id
};

} // namespace forge
