#include "forge/scene/serialization.hpp"
#include "forge/utils/json.hpp"
#include "forge/assets/assets.hpp"
#include "forge/core/logger.hpp"

namespace forge {

namespace {

json::Value vec2Json(Vec2 v) {
    json::Value a = json::Value::array();
    a.push(json::Value((double)v.x));
    a.push(json::Value((double)v.y));
    return a;
}

Vec2 jsonVec2(const json::Value& v, Vec2 def = {0, 0}) {
    if (v.isArray() && v.size() >= 2) return {v.at(0).asFloat(), v.at(1).asFloat()};
    return def;
}

json::Value colorJson(Color c) {
    json::Value a = json::Value::array();
    a.push(json::Value((double)c.r));
    a.push(json::Value((double)c.g));
    a.push(json::Value((double)c.b));
    a.push(json::Value((double)c.a));
    return a;
}

Color jsonColor(const json::Value& v, Color def = Color::white()) {
    if (v.isArray() && v.size() >= 4)
        return {v.at(0).asFloat(), v.at(1).asFloat(), v.at(2).asFloat(), v.at(3).asFloat()};
    return def;
}

} // namespace

json::Value entityToJson(const Scene& scene, Entity e, const AssetManager* assets) {
    (void)assets;
    const Registry& reg = scene.registry();
    json::Value out = json::Value::object();

    if (const Tag* tag = reg.tryGet<Tag>(e))
        out.set("name", json::Value(tag->name));
    if (const Transform* t = reg.tryGet<Transform>(e)) {
        json::Value tr = json::Value::object();
        tr.set("pos", vec2Json(t->pos));
        tr.set("rotation", json::Value((double)t->rotation));
        tr.set("scale", vec2Json(t->scale));
        out.set("transform", std::move(tr));
    }
    if (const Sprite* s = reg.tryGet<Sprite>(e)) {
        json::Value sp = json::Value::object();
        sp.set("texture", json::Value(s->texture ? s->texture->path() : std::string()));
        json::Value src = json::Value::array();
        src.push(json::Value((double)s->src.x));
        src.push(json::Value((double)s->src.y));
        src.push(json::Value((double)s->src.w));
        src.push(json::Value((double)s->src.h));
        sp.set("src", std::move(src));
        sp.set("color", colorJson(s->color));
        sp.set("layer", json::Value(s->layer));
        sp.set("visible", json::Value(s->visible));
        sp.set("flipX", json::Value(s->flipX));
        sp.set("flipY", json::Value(s->flipY));
        sp.set("size", vec2Json(s->size));
        out.set("sprite", std::move(sp));
    }
    if (const Velocity* v = reg.tryGet<Velocity>(e)) {
        json::Value ve = json::Value::object();
        ve.set("v", vec2Json(v->v));
        ve.set("accel", vec2Json(v->accel));
        ve.set("damping", json::Value((double)v->damping));
        ve.set("maxSpeed", json::Value((double)v->maxSpeed));
        out.set("velocity", std::move(ve));
    }
    if (const ScriptRef* sr = reg.tryGet<ScriptRef>(e))
        out.set("script", json::Value(sr->name));
    if (const Collider* c = reg.tryGet<Collider>(e)) {
        json::Value co = json::Value::object();
        co.set("halfExtents", vec2Json(c->halfExtents));
        co.set("solid", json::Value(c->solid));
        co.set("sensor", json::Value(c->sensor));
        co.set("kinematic", json::Value(c->kinematic));
        co.set("friction", json::Value((double)c->friction));
        co.set("gravityScale", json::Value((double)c->gravityScale));
        out.set("collider", std::move(co));
    }
    return out;
}

Entity entityFromJson(Scene& scene, const json::Value& value, Vec2 posOverride,
                      AssetManager* assets, bool hasPosOverride) {
    if (!value.isObject()) return kNullEntity;
    std::string name = value["name"].asString();
    Entity e = scene.create(name);

    const json::Value& tr = value["transform"];
    if (tr.isObject()) {
        Transform t;
        t.pos = hasPosOverride ? posOverride : jsonVec2(tr["pos"]);
        t.rotation = tr["rotation"].asFloat(0);
        t.scale = jsonVec2(tr["scale"], {1, 1});
        scene.registry().emplace<Transform>(e, t);
    } else if (hasPosOverride) {
        scene.registry().emplace<Transform>(e, Transform{posOverride, 0, {1, 1}});
    }

    const json::Value& sp = value["sprite"];
    if (sp.isObject()) {
        Sprite s;
        std::string texPath = sp["texture"].asString();
        if (assets && !texPath.empty()) s.texture = assets->loadTexture(texPath);
        const json::Value& src = sp["src"];
        if (src.isArray() && src.size() >= 4)
            s.src = {src.at(0).asFloat(), src.at(1).asFloat(),
                     src.at(2).asFloat(), src.at(3).asFloat()};
        s.color = jsonColor(sp["color"]);
        s.layer = sp["layer"].asInt(0);
        s.visible = sp["visible"].asBool(true);
        s.flipX = sp["flipX"].asBool(false);
        s.flipY = sp["flipY"].asBool(false);
        s.size = jsonVec2(sp["size"]);
        scene.registry().emplace<Sprite>(e, s);
    }

    const json::Value& ve = value["velocity"];
    if (ve.isObject()) {
        Velocity v;
        v.v = jsonVec2(ve["v"]);
        v.accel = jsonVec2(ve["accel"]);
        v.damping = ve["damping"].asFloat(0);
        v.maxSpeed = ve["maxSpeed"].asFloat(0);
        scene.registry().emplace<Velocity>(e, v);
    }

    const json::Value& script = value["script"];
    if (script.isString() && !script.asString().empty())
        scene.registry().emplace<ScriptRef>(e, ScriptRef{script.asString()});

    const json::Value& co = value["collider"];
    if (co.isObject()) {
        Collider c;
        c.halfExtents = jsonVec2(co["halfExtents"], {16, 16});
        c.solid = co["solid"].asBool(true);
        c.sensor = co["sensor"].asBool(false);
        c.kinematic = co["kinematic"].asBool(false);
        c.friction = co["friction"].asFloat(0);
        c.gravityScale = co["gravityScale"].asFloat(1);
        scene.registry().emplace<Collider>(e, c);
    }
    return e;
}

bool saveScene(const Scene& scene, const std::string& path, const AssetManager* assets) {
    json::Value root = json::Value::object();
    root.set("format", json::Value("forge.scene"));
    root.set("version", json::Value(1));
    json::Value entities = json::Value::array();

    const Registry& reg = scene.registry();
    reg.view<Transform>([&](Entity e, const Transform&) {
        entities.push(entityToJson(scene, e, assets));
    });
    root.set("entities", std::move(entities));
    if (!json::writeFile(path, root, true)) {
        FORGE_LOG_ERROR("saveScene: cannot write '%s'", path.c_str());
        return false;
    }
    return true;
}

bool loadScene(Scene& scene, const std::string& path, AssetManager* assets,
               SceneSaveStats* stats) {
    json::ParseError err;
    json::Value root = json::parseFile(path, &err);
    if (!err.ok()) {
        FORGE_LOG_ERROR("loadScene '%s': %s", path.c_str(), err.str().c_str());
        return false;
    }
    if (root["format"].asString() != "forge.scene") {
        FORGE_LOG_ERROR("loadScene '%s': not a forge.scene file", path.c_str());
        return false;
    }
    int version = root["version"].asInt(0);
    if (version != 1) {
        FORGE_LOG_ERROR("loadScene '%s': unsupported version %d", path.c_str(), version);
        return false;
    }
    scene.clear();
    SceneSaveStats local;
    const json::Value& entities = root["entities"];
    for (size_t i = 0; i < entities.size(); ++i) {
        Entity e = entityFromJson(scene, entities.at(i), {0, 0}, assets, false);
        if (e == kNullEntity) continue;
        ++local.entities;
        if (scene.registry().has<Sprite>(e)) ++local.sprites;
        if (scene.registry().has<Collider>(e)) ++local.colliders;
    }
    if (stats) *stats = local;
    return true;
}

// ------------------------------------------------------------------ prefabs
Prefab makePrefab(const Scene& scene, Entity e, const std::string& name) {
    Prefab p;
    p.name = name;
    p.data = entityToJson(scene, e, nullptr);
    p.data.set("name", json::Value(name));
    return p;
}

bool savePrefab(const Prefab& prefab, const std::string& path) {
    json::Value root = json::Value::object();
    root.set("format", json::Value("forge.prefab"));
    root.set("version", json::Value(1));
    root.set("name", json::Value(prefab.name));
    root.set("entity", prefab.data);
    if (!json::writeFile(path, root, true)) {
        FORGE_LOG_ERROR("savePrefab: cannot write '%s'", path.c_str());
        return false;
    }
    return true;
}

bool loadPrefab(Prefab& prefab, const std::string& path) {
    json::ParseError err;
    json::Value root = json::parseFile(path, &err);
    if (!err.ok()) {
        FORGE_LOG_ERROR("loadPrefab '%s': %s", path.c_str(), err.str().c_str());
        return false;
    }
    if (root["format"].asString() != "forge.prefab") {
        FORGE_LOG_ERROR("loadPrefab '%s': not a forge.prefab file", path.c_str());
        return false;
    }
    prefab.name = root["name"].asString();
    prefab.data = root["entity"];
    return prefab.valid();
}

Entity instantiate(Scene& scene, const Prefab& prefab, Vec2 pos, AssetManager* assets) {
    if (!prefab.valid()) return kNullEntity;
    return entityFromJson(scene, prefab.data, pos, assets, true);
}

} // namespace forge
