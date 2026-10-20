#pragma once
// Scene serialization + prefabs (versioned JSON, no exceptions).
//
// Save format: a top-level object with "format": "forge.scene" and
// "version": 1, then an array of entities. Only data components are
// serialized (Transform, Tag, Sprite, Velocity, Collider, ScriptRef); a
// `Script` closure is code and must be re-attached by the game.
//
//   saveScene(scene, "save.json", &assets);
//   loadScene(scene, "save.json", &assets);
//
// Prefabs are the same entity object without the wrapper, so a prefab file is
// just `{ "name": "...", "components": {...} }`.
//
// Asset paths are stored as the Texture2D's original `path()`; on load the
// AssetManager resolves them (relative to its search paths). If no
// AssetManager is supplied, sprites keep a null texture but keep their rects.

#include "forge/scene/scene.hpp"
#include "forge/utils/json.hpp"
#include <string>

namespace forge {

class AssetManager;

struct SceneSaveStats {
    int entities = 0;
    int sprites = 0;
    int colliders = 0;
};

// Writes atomically (tmp + rename). Returns false on I/O failure.
bool saveScene(const Scene& scene, const std::string& path,
               const AssetManager* assets = nullptr);
bool loadScene(Scene& scene, const std::string& path,
               AssetManager* assets = nullptr, SceneSaveStats* stats = nullptr);

// ---- prefabs ----
struct Prefab {
    std::string name;
    json::Value data; // entity object: { name, components: {...} }

    bool valid() const { return !name.empty(); }
};

Prefab makePrefab(const Scene& scene, Entity e, const std::string& name);
bool savePrefab(const Prefab& prefab, const std::string& path);
bool loadPrefab(Prefab& prefab, const std::string& path);

// Instantiates the prefab at `pos` (overrides the stored Transform position).
// Returns kNullEntity on failure.
Entity instantiate(Scene& scene, const Prefab& prefab, Vec2 pos,
                   AssetManager* assets = nullptr);

// Serializes one entity to a JSON value (used by prefabs and by games that
// want to embed entity data in their own save format).
json::Value entityToJson(const Scene& scene, Entity e, const AssetManager* assets);
Entity entityFromJson(Scene& scene, const json::Value& value, Vec2 posOverride,
                      AssetManager* assets, bool hasPosOverride);

} // namespace forge
