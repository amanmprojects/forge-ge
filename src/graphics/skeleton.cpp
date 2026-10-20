#include "forge/graphics/skeleton.hpp"
#include "forge/graphics/sprite_batch.hpp"
#include "forge/graphics/texture.hpp"
#include "forge/core/logger.hpp"
#include "forge/utils/json.hpp"
#include <algorithm>
#include <cmath>

namespace forge {

namespace {

Vec2 rotateVec(Vec2 v, float a) {
    float c = std::cos(a), s = std::sin(a);
    return {v.x * c - v.y * s, v.x * s + v.y * c};
}

} // namespace

int Skeleton::addBone(const Bone& b) {
    bones_.push_back(b);
    dirty_ = true;
    return (int)bones_.size() - 1;
}

int Skeleton::findBone(const std::string& name) const {
    for (size_t i = 0; i < bones_.size(); ++i)
        if (bones_[i].name == name) return (int)i;
    return -1;
}

Bone* Skeleton::bone(int index) {
    if (index < 0 || index >= (int)bones_.size()) return nullptr;
    dirty_ = true;
    return &bones_[(size_t)index];
}

const Bone* Skeleton::bone(int index) const {
    if (index < 0 || index >= (int)bones_.size()) return nullptr;
    return &bones_[(size_t)index];
}

int Skeleton::addSlot(const Slot& s) {
    Slot copy = s;
    if (copy.bone < 0 && !copy.boneName.empty()) copy.bone = findBone(copy.boneName);
    slots_.push_back(copy);
    return (int)slots_.size() - 1;
}

int Skeleton::findSlot(const std::string& name) const {
    for (size_t i = 0; i < slots_.size(); ++i)
        if (slots_[i].name == name) return (int)i;
    return -1;
}

Slot* Skeleton::slot(int index) {
    if (index < 0 || index >= (int)slots_.size()) return nullptr;
    return &slots_[(size_t)index];
}

bool Skeleton::setSlotTexture(const std::string& slotName, const Texture2D* tex) {
    int i = findSlot(slotName);
    if (i < 0) return false;
    slots_[(size_t)i].texture = tex;
    return true;
}

bool Skeleton::setSlotTexture(int slotIndex, const Texture2D* tex) {
    if (slotIndex < 0 || slotIndex >= (int)slots_.size()) return false;
    slots_[(size_t)slotIndex].texture = tex;
    return true;
}

int Skeleton::addAnimation(const Animation& a) {
    animations_.push_back(a);
    return (int)animations_.size() - 1;
}

int Skeleton::findAnimation(const std::string& name) const {
    for (size_t i = 0; i < animations_.size(); ++i)
        if (animations_[i].name == name) return (int)i;
    return -1;
}

const Animation* Skeleton::animation(int index) const {
    if (index < 0 || index >= (int)animations_.size()) return nullptr;
    return &animations_[(size_t)index];
}

bool Skeleton::play(const std::string& name, bool restart) {
    int idx = findAnimation(name);
    if (idx < 0) return false;
    if (idx == current_ && !restart) return true;
    current_ = idx;
    time_ = 0.0f;
    dirty_ = true;
    return true;
}

void Skeleton::stop() {
    current_ = -1;
    time_ = 0.0f;
    // Drop the sampled pose so the next draw shows the authored rest pose
    // instead of freezing on the last animated frame.
    for (auto& b : bones_) {
        b.pos = b.restPos;
        b.rotation = b.restRotation;
        b.scale = b.restScale;
    }
    dirty_ = true;
}

const std::string& Skeleton::currentAnimation() const {
    static const std::string empty;
    if (current_ < 0 || current_ >= (int)animations_.size()) return empty;
    return animations_[(size_t)current_].name;
}

void Skeleton::sampleTrack(const Track& t, float time, Vec2& out) const {
    if (t.keys.empty()) return;
    if (t.keys.size() == 1 || time <= t.keys.front().time) {
        out = t.keys.front().value;
        return;
    }
    if (time >= t.keys.back().time) {
        out = t.keys.back().value;
        return;
    }
    // Keys are authored in ascending time order; linear scan is fine for the
    // small key counts 2D animations use, and the cursor makes playback O(1).
    size_t i = std::min(t.cursor, t.keys.size() - 1);
    while (i > 0 && t.keys[i].time > time) --i;
    while (i + 1 < t.keys.size() && t.keys[i + 1].time <= time) ++i;
    t.cursor = i;
    const Keyframe& a = t.keys[i];
    const Keyframe& b = t.keys[i + 1];
    float span = b.time - a.time;
    float u = span > 0 ? (time - a.time) / span : 0.0f;
    out = a.value + (b.value - a.value) * u;
}

void Skeleton::update(float dt) {
    if (current_ >= 0 && current_ < (int)animations_.size()) {
        Animation& a = animations_[(size_t)current_];
        time_ += dt * speed_;
        if (a.duration > 0) {
            if (a.loop) {
                time_ = std::fmod(time_, a.duration);
                if (time_ < 0) time_ += a.duration;
            } else if (time_ > a.duration) {
                time_ = a.duration;
            }
        }
        // Restore the authored rest pose, then layer animation tracks on top
        // (a missing track keeps the authored pose instead of collapsing).
        for (auto& b : bones_) {
            b.pos = b.restPos;
            b.rotation = b.restRotation;
            b.scale = b.restScale;
        }
        for (auto& t : a.tracks) {
            if (t.bone < 0 || t.bone >= (int)bones_.size()) continue;
            Vec2 v;
            sampleTrack(t, time_, v);
            Bone& b = bones_[(size_t)t.bone];
            switch (t.type) {
                case TrackType::Translation: b.pos = v; break;
                case TrackType::Rotation: b.rotation = v.x; break;
                case TrackType::Scale: b.scale = v; break;
            }
        }
        dirty_ = true;
    }
    applyPose();
}

void Skeleton::applyPose() {
    for (size_t i = 0; i < bones_.size(); ++i) {
        Bone& b = bones_[i];
        if (b.parent >= 0 && b.parent < (int)i) {
            const Bone& p = bones_[(size_t)b.parent];
            Vec2 scaled{b.pos.x * p.worldScale.x, b.pos.y * p.worldScale.y};
            b.worldPos = p.worldPos + rotateVec(scaled, p.worldRotation);
            b.worldRotation = p.worldRotation + b.rotation;
            b.worldScale = {p.worldScale.x * b.scale.x, p.worldScale.y * b.scale.y};
        } else {
            b.worldPos = b.pos;
            b.worldRotation = b.rotation;
            b.worldScale = b.scale;
        }
    }
    dirty_ = false;
}

void Skeleton::draw(SpriteBatch& batch, Vec2 origin, float rotation,
                    Vec2 scale, bool flipX) const {
    if (dirty_) const_cast<Skeleton*>(this)->applyPose();
    std::vector<const Slot*> order;
    order.reserve(slots_.size());
    for (auto& s : slots_)
        if (s.visible && s.texture && s.texture->valid() && s.bone >= 0) order.push_back(&s);
    std::stable_sort(order.begin(), order.end(),
                     [](const Slot* a, const Slot* b) { return a->drawOrder < b->drawOrder; });

    for (const Slot* s : order) {
        const Bone& b = bones_[(size_t)s->bone];
        Vec2 local{s->offset.x * b.worldScale.x, s->offset.y * b.worldScale.y};
        Vec2 wp = b.worldPos + rotateVec(local, b.worldRotation);
        // Apply the skeleton root transform, then optional facing mirror.
        Vec2 p{origin.x + (flipX ? -wp.x : wp.x) * scale.x,
               origin.y + wp.y * scale.y};
        float rot = rotation + b.worldRotation + s->rotation;
        SpriteDraw d;
        d.tex = s->texture;
        d.pos = p;
        d.size = {s->size.x * b.worldScale.x * scale.x * (flipX ? -1.0f : 1.0f),
                  s->size.y * b.worldScale.y * scale.y};
        d.src = s->src;
        d.rotation = rot;
        d.color = s->tint;
        batch.draw(d);
    }
}

void Skeleton::debugDraw(SpriteBatch& batch, Vec2 origin, float rotation, Vec2 scale) const {
    if (dirty_) const_cast<Skeleton*>(this)->applyPose();
    (void)rotation;
    for (auto& b : bones_) {
        Vec2 p{origin.x + b.worldPos.x * scale.x, origin.y + b.worldPos.y * scale.y};
        if (b.parent >= 0 && b.parent < (int)bones_.size()) {
            const Bone& par = bones_[(size_t)b.parent];
            Vec2 pp{origin.x + par.worldPos.x * scale.x, origin.y + par.worldPos.y * scale.y};
            batch.drawLine(pp, p, Color::fromHex(0x00E5FFFF), 1.5f);
        }
        batch.drawCircle(p, 3.0f, Color::fromHex(0xFF4081FF));
    }
}

// ------------------------------------------------------------------- JSON
bool Skeleton::loadJson(const std::string& path) {
    json::ParseError err;
    json::Value root = json::parseFile(path, &err);
    if (!err.ok()) {
        FORGE_LOG_ERROR("Skeleton '%s': %s", path.c_str(), err.str().c_str());
        return false;
    }
    return parseJson(json::write(root), path);
}

bool Skeleton::parseJson(const std::string& text, const std::string& sourceName) {
    json::ParseError err;
    json::Value root = json::parse(text, &err);
    if (!err.ok()) {
        FORGE_LOG_ERROR("Skeleton '%s': %s", sourceName.c_str(), err.str().c_str());
        return false;
    }
    bones_.clear();
    slots_.clear();
    animations_.clear();
    current_ = -1;

    const json::Value& jbones = root["bones"];
    for (size_t i = 0; i < jbones.size(); ++i) {
        const json::Value& jb = jbones.at(i);
        Bone b;
        b.name = jb["name"].asString();
        if (b.name.empty()) b.name = "bone" + std::to_string(i);
        std::string parentName = jb["parent"].asString();
        if (!parentName.empty()) b.parent = findBone(parentName);
        b.pos = {jb["x"].asFloat(0), jb["y"].asFloat(0)};
        b.rotation = jb["rotation"].asFloat(0);
        b.scale = {jb["scaleX"].asFloat(1), jb["scaleY"].asFloat(1)};
        b.length = jb["length"].asFloat(0);
        b.restPos = b.pos;
        b.restRotation = b.rotation;
        b.restScale = b.scale;
        bones_.push_back(b);
    }

    const json::Value& jslots = root["slots"];
    for (size_t i = 0; i < jslots.size(); ++i) {
        const json::Value& js = jslots.at(i);
        Slot s;
        s.name = js["name"].asString();
        s.boneName = js["bone"].asString();
        s.bone = findBone(s.boneName);
        s.textureName = js["texture"].asString();
        s.src = {js["srcX"].asFloat(0), js["srcY"].asFloat(0),
                 js["srcW"].asFloat(1), js["srcH"].asFloat(1)};
        s.size = {js["width"].asFloat(32), js["height"].asFloat(32)};
        s.offset = {js["offsetX"].asFloat(0), js["offsetY"].asFloat(0)};
        s.rotation = js["rotation"].asFloat(0);
        s.drawOrder = js["order"].asInt((int)i);
        s.tint = Color{js["r"].asFloat(1), js["g"].asFloat(1),
                       js["b"].asFloat(1), js["a"].asFloat(1)};
        if (s.bone >= 0) slots_.push_back(std::move(s));
    }

    const json::Value& janims = root["animations"];
    for (size_t i = 0; i < janims.size(); ++i) {
        const json::Value& ja = janims.at(i);
        Animation a;
        a.name = ja["name"].asString();
        a.duration = ja["duration"].asFloat(1.0f);
        a.loop = ja["loop"].asBool(true);
        const json::Value& jtracks = ja["tracks"];
        for (size_t ti = 0; ti < jtracks.size(); ++ti) {
            const json::Value& jt = jtracks.at(ti);
            Track t;
            t.bone = findBone(jt["bone"].asString());
            std::string kind = jt["type"].asString();
            if (kind == "translation") t.type = TrackType::Translation;
            else if (kind == "scale") t.type = TrackType::Scale;
            else t.type = TrackType::Rotation;
            const json::Value& keys = jt["keys"];
            for (size_t ki = 0; ki < keys.size(); ++ki) {
                const json::Value& k = keys.at(ki);
                Keyframe kf;
                kf.time = k["time"].asFloat(0);
                kf.value = {k["x"].asFloat(0), k["y"].asFloat(0)};
                // Rotation keys use `angle`; accept `x` as a friendly alias so
                // the same key object shape works for every track type.
                if (t.type == TrackType::Rotation)
                    kf.value.x = k["angle"].asFloat(k["x"].asFloat(0));
                t.keys.push_back(kf);
            }
            std::stable_sort(t.keys.begin(), t.keys.end(),
                             [](const Keyframe& a2, const Keyframe& b2) { return a2.time < b2.time; });
            if (t.bone >= 0 && !t.keys.empty()) a.tracks.push_back(std::move(t));
        }
        animations_.push_back(std::move(a));
    }
    dirty_ = true;
    return valid();
}

} // namespace forge
