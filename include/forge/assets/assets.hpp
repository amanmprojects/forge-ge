#pragma once
// AssetManager: cached textures/fonts/sounds with hot-reload (mtime check).
// update() throttles filesystem checks to ~2 Hz; hotReload() forces a check.

#include "forge/graphics/texture.hpp"
#include "forge/graphics/font.hpp"
#include "forge/graphics/shader.hpp"
#include "forge/audio/audio.hpp"
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <chrono>

namespace forge {

class AssetManager {
public:
    explicit AssetManager(AudioEngine* audio = nullptr);
    ~AssetManager() = default;

    void setAudio(AudioEngine* a) { audio_ = a; }

    // ---- search paths ----
    // Relative asset paths are resolved against these directories, in order,
    // then against the path as given. Lets a game run from the repo root, from
    // a build directory, or from an installed bundle without code changes.
    void addSearchPath(const std::string& dir);
    void clearSearchPaths() { searchPaths_.clear(); }
    const std::vector<std::string>& searchPaths() const { return searchPaths_; }
    // Resolves `path` to the first existing candidate, or `path` unchanged.
    std::string resolve(const std::string& path) const;

    // Returns cached instance; loads on first use. nullptr/empty on failure.
    // NOTE: returned pointers are stable (heap-allocated) until unloadAll/clear.
    Texture2D* loadTexture(const std::string& path,
                           TextureFilter filter = TextureFilter::Linear);
    Font* loadFont(const std::string& path, float pxHeight = 32.0f);
    SoundHandle loadSound(const std::string& path);
    // Shader program from two files, cached and hot-reloaded on mtime change.
    // A failed (re)load keeps the previous program alive.
    Shader* loadShader(const std::string& vertPath, const std::string& fragPath);

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
    struct ShaderEntry {
        std::unique_ptr<Shader> shader;
        std::string vertPath;
        std::string fragPath;
        int64_t vertMtime = 0;
        int64_t fragMtime = 0;
    };
    static int64_t fileMtime(const std::string& path);

    AudioEngine* audio_ = nullptr;
    std::vector<std::string> searchPaths_;
    std::unordered_map<std::string, TexEntry> textures_;
    std::unordered_map<std::string, ShaderEntry> shaders_;
    std::unordered_map<std::string, FontEntry> fonts_;
    std::unordered_map<std::string, SoundEntry> sounds_;
    bool hotReload_ = true;
    std::chrono::steady_clock::time_point lastCheck_{};
};

} // namespace forge
