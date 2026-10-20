#pragma once
// Skeleton: 2D bone hierarchy + slot attachments + keyframe animation.
//
// Deliberately small and data-driven:
// - bones form a tree (parent index, local translation/rotation/scale)
// - slots attach a texture region to a bone (offset, rotation, tint)
// - animations are per-bone translation/rotation/scale tracks sampled linearly
// - JSON loading via forge::utils/json (no editor format, no IK, no meshes)
//
//   Skeleton sk;
//   sk.loadJson("hero.skel.json");
//   sk.setSlotTexture("body", tex);
//   sk.play("walk");
//   sk.update(dt);
//   sk.draw(batch, worldPos, facing);
//
// Slot textures are resolved by name through the AssetManager in game code;
// the skeleton never owns textures.

#include "forge/math/math.hpp"
#include <string>
#include <vector>

namespace forge {

class SpriteBatch;
class Texture2D;

struct Bone {
    std::string name;
    int parent = -1;
    Vec2 pos{0, 0};        // local translation (animated)
    float rotation = 0.0f; // local radians (animated)
    Vec2 scale{1, 1};
    float length = 0.0f;   // for debug drawing only
    // Authored rest pose, restored before each animation sample.
    Vec2 restPos{0, 0};
    float restRotation = 0.0f;
    Vec2 restScale{1, 1};
    // Computed by update().
    Vec2 worldPos{0, 0};
    float worldRotation = 0.0f;
    Vec2 worldScale{1, 1};
};

struct Slot {
    std::string name;
    std::string boneName;
    int bone = -1;
    const Texture2D* texture = nullptr;
    std::string textureName;
    Rect src{0, 0, 1, 1};   // UV rect (0..1)
    Vec2 size{32, 32};      // world size at scale 1
    Vec2 offset{0, 0};      // local offset from the bone
    float rotation = 0.0f;  // local rotation
    Color tint = Color::white();
    int drawOrder = 0;      // lower draws first
    bool visible = true;
};

enum class TrackType { Translation, Rotation, Scale };

struct Keyframe {
    float time = 0.0f;
    Vec2 value{0, 0}; // Rotation uses value.x
};

struct Track {
    int bone = -1;
    TrackType type = TrackType::Rotation;
    std::vector<Keyframe> keys;
    // Sampling cache (binary search start index), mutated by sample().
    mutable size_t cursor = 0;
};

struct Animation {
    std::string name;
    float duration = 1.0f;
    bool loop = true;
    std::vector<Track> tracks;
};

class Skeleton {
public:
    Skeleton() = default;

    // Parses the JSON skeleton format (see docs/agents/05-animation-particles.md).
    bool loadJson(const std::string& path);
    bool parseJson(const std::string& text, const std::string& sourceName = "<memory>");
    bool valid() const { return !bones_.empty(); }

    // ---- bones ----
    int addBone(const Bone& b);
    int findBone(const std::string& name) const;
    Bone* bone(int index);
    const Bone* bone(int index) const;
    size_t boneCount() const { return bones_.size(); }

    // ---- slots ----
    int addSlot(const Slot& s);
    int findSlot(const std::string& name) const;
    Slot* slot(int index);
    size_t slotCount() const { return slots_.size(); }
    bool setSlotTexture(const std::string& slotName, const Texture2D* tex);
    bool setSlotTexture(int slotIndex, const Texture2D* tex);
    // Resolves every slot by name using a caller-provided lookup.
    template <typename Fn>
    void resolveTextures(Fn&& lookup) {
        for (auto& s : slots_) {
            if (!s.texture && !s.textureName.empty()) s.texture = lookup(s.textureName);
        }
    }

    // ---- animation ----
    int addAnimation(const Animation& a);
    int findAnimation(const std::string& name) const;
    size_t animationCount() const { return animations_.size(); }
    const Animation* animation(int index) const;
    // Plays by name. Unknown names are ignored (returns false).
    bool play(const std::string& name, bool restart = true);
    void stop();
    void setSpeed(float s) { speed_ = s; }
    float speed() const { return speed_; }
    bool playing() const { return current_ >= 0; }
    float time() const { return time_; }
    const std::string& currentAnimation() const;

    // Advances the animation and recomputes world transforms.
    void update(float dt);
    // Recomputes world transforms without advancing time.
    void applyPose();

    // Draws slots in drawOrder, transformed by (origin, rotation, scale) and
    // mirrored horizontally when `flipX` is set (facing left).
    void draw(SpriteBatch& batch, Vec2 origin, float rotation = 0.0f,
              Vec2 scale = {1, 1}, bool flipX = false) const;

    // Debug wireframe (bones as lines + joints as circles).
    void debugDraw(SpriteBatch& batch, Vec2 origin, float rotation = 0.0f,
                   Vec2 scale = {1, 1}) const;

private:
    void sampleTrack(const Track& t, float time, Vec2& out) const;

    std::vector<Bone> bones_;
    std::vector<Slot> slots_;
    std::vector<Animation> animations_;
    int current_ = -1;
    float time_ = 0.0f;
    float speed_ = 1.0f;
    bool dirty_ = true;
};

} // namespace forge
