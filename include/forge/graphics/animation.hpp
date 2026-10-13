#pragma once
// Animation: spritesheet clips + animator with playback speed, looping,
// frame events. Tween: fire-and-forget value interpolation with easing.

#include "forge/math/math.hpp"
#include "forge/graphics/texture.hpp"
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>

namespace forge {

class SpriteBatch;

// A Clip is a row (or explicit list) of frames inside one texture atlas.
struct AnimClip {
    std::string name;
    std::vector<Rect> frames;   // pixel rects in atlas
    float fps = 12.0f;
    bool loop = true;

    static AnimClip gridRow(const std::string& name, int row, int cols,
                            int frameW, int frameH, float fps = 12.0f, bool loop = true);
    static AnimClip grid(const std::string& name, int cols, int rows,
                         int frameW, int frameH, float fps = 12.0f, bool loop = true);
    float duration() const { return frames.empty() ? 0 : frames.size() / fps; }
};

class Animator {
public:
    void addClip(const AnimClip& clip);
    bool play(const std::string& name, bool restart = false);
    void stop() { playing_ = false; }
    void pause() { paused_ = true; }
    void resume() { paused_ = false; }
    void update(float dt);

    const AnimClip* current() const { return cur_; }
    int frameIndex() const { return frame_; }
    Rect currentFrame() const;
    bool finished() const;      // non-looping clip reached end
    bool playing() const { return playing_ && !paused_; }
    void setSpeed(float s) { speed_ = s; }
    void setFrameEvent(int frame, std::function<void()> cb) { events_[frame] = std::move(cb); }

    void draw(SpriteBatch& batch, const Texture2D& tex, Vec2 pos, Vec2 size,
              float rot = 0, Color c = Color::white(), bool flipX = false) const;

private:
    std::unordered_map<std::string, AnimClip> clips_;
    const AnimClip* cur_ = nullptr;
    float time_ = 0;
    int frame_ = 0;
    bool playing_ = false, paused_ = false;
    float speed_ = 1.0f;
    bool fired_ = false;
    std::unordered_map<int, std::function<void()>> events_;
};

// ---------------------------------------------------------------- Tween
enum class TweenMode { Once, Loop, PingPong };

struct Tween {
    float from = 0, to = 1, duration = 1;
    float elapsed = 0;
    TweenMode mode = TweenMode::Once;
    float (*ease)(float) = easing::outCubic;
    std::function<void(float)> onUpdate;
    std::function<void()> onDone;
    bool done = false;

    void update(float dt);
    float value() const;
    void reset() { elapsed = 0; done = false; }
};

class Tweens {
public:
    int add(const Tween& t);
    void update(float dt);
    void clear() { items_.clear(); }
    bool empty() const { return items_.empty(); }

private:
    std::vector<Tween> items_;
};

} // namespace forge
