#pragma once
// Particles: CPU-simulated pool (SoA), rendered through SpriteBatch.
// Zero allocations after init; O(n) update, swap-remove dead.

#include "forge/math/math.hpp"
#include <cstdint>
#include <vector>

namespace forge {

class SpriteBatch;
class Texture2D;

struct Particle {
    Vec2 pos{0, 0};
    Vec2 vel{0, 0};
    Vec2 accel{0, 0};
    float life = 0;        // remaining
    float maxLife = 1;
    float size = 8;
    float endSize = 0;
    float rotation = 0;
    float rotVel = 0;
    Color start = Color::white();
    Color end = Color::white();
    float damping = 0;     // exponential drag
};

struct EmitterConfig {
    const Texture2D* texture = nullptr; // null = white square
    int maxParticles = 1024;
    float spawnRate = 100.0f;   // particles/sec
    bool looping = true;

    Vec2 posVar{0, 0};          // spawn box around emitter pos
    Vec2 vel{0, 100};           // base velocity
    Vec2 velVar{40, 40};        // random spread
    Vec2 accel{0, 0};           // gravity etc.
    float life = 1.0f, lifeVar = 0.3f;
    float size = 8.0f, sizeVar = 3.0f, endSize = 0.0f;
    Color start = Color::white(), end = Color::white();
    float damping = 0.0f;
    float rotVel = 0.0f, rotVelVar = 0.0f;
    uint64_t seed = 12345;
};

class ParticleSystem {
public:
    ParticleSystem() = default;
    explicit ParticleSystem(const EmitterConfig& cfg) { configure(cfg); }

    void configure(const EmitterConfig& cfg);
    void setPosition(Vec2 p) { origin_ = p; }
    Vec2 position() const { return origin_; }

    void emit(int count);           // burst
    void emitOne();
    void update(float dt);          // simulate + auto-spawn if looping
    void draw(SpriteBatch& batch) const;

    void clear() { count_ = 0; }
    int alive() const { return count_; }
    int capacity() const { return (int)pool_.size(); }
    void setEmitting(bool e) { emitting_ = e; }
    bool emitting() const { return emitting_; }

private:
    EmitterConfig cfg_;
    std::vector<Particle> pool_;
    int count_ = 0;
    Vec2 origin_{0, 0};
    float spawnAccum_ = 0;
    bool emitting_ = true;
    Random rng_{12345};
};

} // namespace forge
