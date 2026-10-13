#include "forge/graphics/animation.hpp"
#include "forge/graphics/sprite_batch.hpp"

namespace forge {

AnimClip AnimClip::gridRow(const std::string& name, int row, int cols,
                           int frameW, int frameH, float fps, bool loop) {
    AnimClip c;
    c.name = name; c.fps = fps; c.loop = loop;
    for (int i = 0; i < cols; ++i)
        c.frames.push_back({(float)i * frameW, (float)row * frameH, (float)frameW, (float)frameH});
    return c;
}

AnimClip AnimClip::grid(const std::string& name, int cols, int rows,
                        int frameW, int frameH, float fps, bool loop) {
    AnimClip c;
    c.name = name; c.fps = fps; c.loop = loop;
    for (int r = 0; r < rows; ++r)
        for (int i = 0; i < cols; ++i)
            c.frames.push_back({(float)i * frameW, (float)r * frameH, (float)frameW, (float)frameH});
    return c;
}

void Animator::addClip(const AnimClip& clip) {
    clips_[clip.name] = clip;
}

bool Animator::play(const std::string& name, bool restart) {
    auto it = clips_.find(name);
    if (it == clips_.end()) return false;
    if (cur_ == &it->second && !restart) { playing_ = true; paused_ = false; return true; }
    cur_ = &it->second;
    time_ = 0; frame_ = 0; fired_ = false;
    playing_ = true; paused_ = false;
    return true;
}

void Animator::update(float dt) {
    if (!playing_ || paused_ || !cur_ || cur_->frames.empty()) return;
    time_ += dt * speed_;
    float dur = cur_->duration();
    if (cur_->loop) {
        while (time_ >= dur && dur > 0) time_ -= dur;
    } else if (time_ >= dur) {
        time_ = dur;
    }
    int f = (int)(time_ * cur_->fps);
    if (f >= (int)cur_->frames.size()) f = (int)cur_->frames.size() - 1;
    if (f != frame_) {
        frame_ = f;
        auto ev = events_.find(frame_);
        if (ev != events_.end() && ev->second) ev->second();
    }
}

Rect Animator::currentFrame() const {
    if (!cur_ || cur_->frames.empty()) return {0, 0, 0, 0};
    int f = frame_;
    if (f < 0) f = 0;
    if (f >= (int)cur_->frames.size()) f = (int)cur_->frames.size() - 1;
    return cur_->frames[(size_t)f];
}

bool Animator::finished() const {
    if (!cur_ || cur_->loop) return false;
    return time_ >= cur_->duration();
}

void Animator::draw(SpriteBatch& batch, const Texture2D& tex, Vec2 pos, Vec2 size,
                    float rot, Color c, bool flipX) const {
    if (!cur_ || cur_->frames.empty()) return;
    SpriteDraw d;
    d.tex = &tex;
    d.pos = pos; d.size = size; d.rotation = rot; d.color = c;
    d.flipX = flipX;
    Rect px = currentFrame();
    float tw = (float)tex.width(), th = (float)tex.height();
    if (tw > 0 && th > 0)
        d.src = {px.x / tw, 1.0f - (px.y + px.h) / th, px.w / tw, px.h / th};
    batch.draw(d);
}

// ---------------------------------------------------------------- Tween
void Tween::update(float dt) {
    if (done) return;
    elapsed += dt;
    float t = duration > 0 ? elapsed / duration : 1.0f;
    if (t >= 1.0f) {
        if (mode == TweenMode::Once) {
            done = true;
            if (onUpdate) onUpdate(ease ? ease(1.0f) : 1.0f);
            if (onDone) onDone();
            return;
        } else if (mode == TweenMode::Loop) {
            elapsed = 0;
        } else { // PingPong
            std::swap(from, to);
            elapsed = 0;
        }
    }
    if (onUpdate) onUpdate(value());
}

float Tween::value() const {
    float t = duration > 0 ? clamp01(elapsed / duration) : 1.0f;
    float e = ease ? ease(t) : t;
    return lerp(from, to, e);
}

int Tweens::add(const Tween& t) {
    items_.push_back(t);
    return (int)items_.size() - 1;
}

void Tweens::update(float dt) {
    for (size_t i = 0; i < items_.size();) {
        items_[i].update(dt);
        if (items_[i].done && items_[i].mode == TweenMode::Once)
            items_[i] = std::move(items_.back()), items_.pop_back();
        else
            ++i;
    }
}

} // namespace forge
