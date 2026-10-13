#pragma once
// AssetManager: cached textures/fonts/sounds with hot-reload (mtime check).
// update() throttles filesystem checks to ~2 Hz; hotReload() forces a check.

#include "forge/graphics/texture.hpp"
#include "forge/graphics/font.hpp"
#include "forge/audio/audio.hpp"
#include <string>
#include <unordered_map>
#include <memory>
#include <chrono>

namespace forge {

class AssetManager {
public:
    explicit AssetManager(AudioEngine* audio = nullptr);
    ~AssetManager() = default;

    void setAudio(AudioEngine* a) { audio_ = a; }

    // Returns cached instance; loads on first use. nullptr/empty on failure.
    // NOTE: returned pointers are stable (heap-allocated) until unloadAll/clear.
    Texture2D* loadTexture(const std::string& path,
                           TextureFilter filter = TextureFilter::Linear);
    Font* loadFont(const std::string& path, float pxHeight = 32.0f);
    SoundHandle loadSound(const std::string& path);

    void unloadTexture(const std::string& path);
    void clear(); // unload everything

    // Hot reload: re-load assets whose mtime changed. Returns count.
    // Covers textures, fonts AND sounds (sounds re-decode in place).
    int hotReload();

    void setHotReload(bool on) { hotReload_ = on; }
    void update(); // throttled hotReload() if enabled

    size_t textureCount() const { return textures_.size(); }

private:
    struct TexEntry {
        std::unique_ptr<Texture2D> tex;
        int64_t mtime = 0; // nanoseconds (Linux) — catches sub-second saves
        TextureFilter filter = TextureFilter::Linear;
    };
    struct FontEntry {
        std::unique_ptr<Font> font;
        int64_t mtime = 0;
        float px = 32;
    };
    struct SoundEntry {
        SoundHandle handle;
        int64_t mtime = 0;
    };
    static int64_t fileMtime(const std::string& path);

    AudioEngine* audio_ = nullptr;
    std::unordered_map<std::string, TexEntry> textures_;
    std::unordered_map<std::string, FontEntry> fonts_;
    std::unordered_map<std::string, SoundEntry> sounds_;
    bool hotReload_ = true;
    std::chrono::steady_clock::time_point lastCheck_{};
};

} // namespace forge
