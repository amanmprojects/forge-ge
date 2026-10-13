#include "forge/scene/scene.hpp"
#include "forge/graphics/sprite_batch.hpp"
#include <algorithm>
#include <vector>

namespace forge {

Entity Scene::create(const std::string& tag) {
    Entity e = reg_.create();
    reg_.emplace<Transform>(e);
    if (!tag.empty()) reg_.emplace<Tag>(e, Tag{tag});
    return e;
}

void Scene::destroy(Entity e) { reg_.destroy(e); }
void Scene::clear() { reg_.clear(); }

void Scene::update(float dt) {
    // Snapshot script owners first: user scripts may spawn/destroy entities,
    // which would invalidate the view iteration (see Registry::view docs).
    std::vector<Entity> scripts;
    reg_.view<Script>([&](Entity e, Script&) { scripts.push_back(e); });
    for (Entity e : scripts) {
        if (!reg_.alive(e)) continue;
        Script* s = reg_.tryGet<Script>(e);
        if (!s) continue;
        if (!s->started) {
            s->started = true;
            if (s->onStart) s->onStart(e, reg_);
            if (!reg_.alive(e)) continue;
        }
        if (s->onUpdate) s->onUpdate(e, reg_, dt);
    }
    // velocity integration
    reg_.view<Transform, Velocity>([dt](Entity, Transform& t, Velocity& v) {
        v.v += v.accel * dt;
        if (v.damping > 0) v.v *= (1.0f - clamp01(v.damping * dt));
        if (v.maxSpeed > 0) {
            float l = v.v.length();
            if (l > v.maxSpeed) v.v *= (v.maxSpeed / l);
        }
        t.pos += v.v * dt;
    });
}

void Scene::draw(SpriteBatch& batch, const Camera2D* cam) {
    (void)cam;
    // collect + sort by layer (insertion-ish order otherwise)
    struct Item { int layer; Entity e; };
    std::vector<Item> items;
    reg_.view<Transform, Sprite>([&](Entity e, Transform&, Sprite& s) {
        if (s.visible) items.push_back({s.layer, e});
    });
    std::stable_sort(items.begin(), items.end(),
        [](const Item& a, const Item& b) { return a.layer < b.layer; });

    for (auto& it : items) {
        Entity e = it.e;
        const Transform& t = reg_.get<Transform>(e);
        const Sprite& s = reg_.get<Sprite>(e);
        SpriteDraw d;
        d.tex = s.texture;
        d.pos = t.pos;
        d.size = (s.size.x > 0 && s.size.y > 0) ? s.size
                 : (s.texture ? s.texture->size() * t.scale : Vec2{32, 32} * t.scale);
        d.rotation = t.rotation;
        d.color = s.color;
        d.flipX = s.flipX; d.flipY = s.flipY;
        if (s.texture && s.src.w > 0 && s.src.h > 0) {
            float tw = (float)s.texture->width(), th = (float)s.texture->height();
            d.src = {s.src.x / tw, 1.0f - (s.src.y + s.src.h) / th, s.src.w / tw, s.src.h / th};
        }
        batch.draw(d);
    }
}

Entity Scene::findByTag(const std::string& tag) {
    Entity found = kNullEntity;
    reg_.view<Tag>([&](Entity e, Tag& t) {
        if (found == kNullEntity && t.name == tag) found = e;
    });
    return found;
}

void Scene::each(const std::function<void(Entity)>& fn) {
    reg_.view<Transform>([&](Entity e, Transform&) { fn(e); });
}

} // namespace forge
