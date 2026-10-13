#pragma once
// AssetManager: cached textures/fonts/sounds with hot-reload (mtime check).
// Call update() once per frame (or every N frames) to reload changed files.

#include "forge/graphics/texture.hpp"
#include "forge/graphics/font.hpp"
#include "forge/audio/audio.hpp"
#include <string>
#include <unordered_map>
#include <memory>

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

    // Hot reload: re-load textures/fonts whose mtime changed. Returns count.
    int hotReload();

    void setHotReload(bool on) { hotReload_ = on; }
    void update(); // calls hotReload() if enabled

    size_t textureCount() const { return textures_.size(); }

private:
    struct TexEntry {
        std::unique_ptr<Texture2D> tex;
        int64_t mtime = 0;
        TextureFilter filter = TextureFilter::Linear;
    };
    struct FontEntry {
        std::unique_ptr<Font> font;
        int64_t mtime = 0;
        float px = 32;
    };
    static int64_t fileMtime(const std::string& path);

    AudioEngine* audio_ = nullptr;
    std::unordered_map<std::string, TexEntry> textures_;
    std::unordered_map<std::string, FontEntry> fonts_;
    std::unordered_map<std::string, SoundHandle> sounds_;
    bool hotReload_ = true;
};

} // namespace forge
