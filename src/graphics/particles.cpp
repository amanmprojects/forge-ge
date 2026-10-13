#include "forge/graphics/particles.hpp"
#include "forge/graphics/sprite_batch.hpp"
#include "forge/graphics/texture.hpp"

namespace forge {

void ParticleSystem::configure(const EmitterConfig& cfg) {
    cfg_ = cfg;
    pool_.resize(cfg.maxParticles > 0 ? cfg.maxParticles : 1);
    count_ = 0;
    spawnAccum_ = 0;
    rng_.seed(cfg.seed ? cfg.seed : 12345);
}

void ParticleSystem::emitOne() {
    if (count_ >= (int)pool_.size()) return; // pool exhausted: drop (no alloc)
    Particle& p = pool_[count_++];
    p.pos = origin_ + Vec2{rng_.range(-cfg_.posVar.x, cfg_.posVar.x),
                           rng_.range(-cfg_.posVar.y, cfg_.posVar.y)};
    p.vel = {cfg_.vel.x + rng_.range(-cfg_.velVar.x, cfg_.velVar.x),
             cfg_.vel.y + rng_.range(-cfg_.velVar.y, cfg_.velVar.y)};
    p.accel = cfg_.accel;
    p.maxLife = p.life = cfg_.life + rng_.range(-cfg_.lifeVar, cfg_.lifeVar);
    if (p.life <= 0.01f) p.life = p.maxLife = 0.01f;
    p.size = cfg_.size + rng_.range(-cfg_.sizeVar, cfg_.sizeVar);
    if (p.size < 0.5f) p.size = 0.5f;
    p.endSize = cfg_.endSize;
    p.rotation = rng_.range(0, TAU);
    p.rotVel = cfg_.rotVel + rng_.range(-cfg_.rotVelVar, cfg_.rotVelVar);
    p.start = cfg_.start;
    p.end = cfg_.end;
    p.damping = cfg_.damping;
}

void ParticleSystem::emit(int count) {
    for (int i = 0; i < count; ++i) emitOne();
}

void ParticleSystem::update(float dt) {
    if (emitting_ && cfg_.looping && cfg_.spawnRate > 0) {
        spawnAccum_ += dt * cfg_.spawnRate;
        int n = (int)spawnAccum_;
        spawnAccum_ -= n;
        for (int i = 0; i < n; ++i) emitOne();
    }
    for (int i = 0; i < count_;) {
        Particle& p = pool_[i];
        p.life -= dt;
        if (p.life <= 0) {
            pool_[i] = pool_[--count_]; // swap-remove
            continue;
        }
        p.vel += p.accel * dt;
        if (p.damping > 0) {
            float k = 1.0f - clamp01(p.damping * dt);
            p.vel *= k;
        }
        p.pos += p.vel * dt;
        p.rotation += p.rotVel * dt;
        ++i;
    }
}

void ParticleSystem::draw(SpriteBatch& batch) const {
    for (int i = 0; i < count_; ++i) {
        const Particle& p = pool_[i];
        float t = 1.0f - p.life / p.maxLife; // 0 fresh -> 1 dead
        SpriteDraw d;
        d.tex = cfg_.texture;
        d.pos = p.pos;
        d.size = {lerp(p.size, p.endSize, t), lerp(p.size, p.endSize, t)};
        d.rotation = p.rotation;
        d.color = p.start.lerped(p.end, t);
        batch.draw(d);
    }
}

} // namespace forge
