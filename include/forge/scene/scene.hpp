#pragma once
// Scene: owns a Registry, updates scripts/velocities, draws sprites sorted
// by layer. Systems are plain functions so games can add their own.

#include "forge/scene/ecs.hpp"
#include "forge/scene/components.hpp"

namespace forge {

class SpriteBatch;
class Camera2D;

class Scene {
public:
    Scene() = default;

    Registry& registry() { return reg_; }
    const Registry& registry() const { return reg_; }

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
    Registry reg_;
};

} // namespace forge
