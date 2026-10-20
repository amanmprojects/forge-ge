#include "forge/assets/assets.hpp"
#include "forge/core/logger.hpp"
#include <sys/stat.h>
#include <chrono>
#include <cstring>

namespace forge {

AssetManager::AssetManager(AudioEngine* audio) : audio_(audio) {}

void AssetManager::addSearchPath(const std::string& dir) {
    if (dir.empty()) return;
    std::string normalized = dir;
    if (normalized.back() != '/') normalized += '/';
    for (auto& p : searchPaths_)
        if (p == normalized) return;
    searchPaths_.push_back(std::move(normalized));
}

std::string AssetManager::resolve(const std::string& path) const {
    if (path.empty()) return path;
    // Absolute paths (and Windows drive paths) bypass the search list.
    if (path[0] == '/' || (path.size() > 1 && path[1] == ':')) return path;
    struct stat st{};
    if (::stat(path.c_str(), &st) == 0) return path;
    for (const auto& dir : searchPaths_) {
        std::string candidate = dir + path;
        if (::stat(candidate.c_str(), &st) == 0) return candidate;
    }
    return path;
}

int64_t AssetManager::fileMtime(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return 0;
#if defined(__linux__)
    return (int64_t)st.st_mtim.tv_sec * 1000000000LL + (int64_t)st.st_mtim.tv_nsec;
#else
    return (int64_t)st.st_mtime * 1000000000LL; // second granularity elsewhere
#endif
}

Texture2D* AssetManager::loadTexture(const std::string& path, TextureFilter filter) {
    auto it = textures_.find(path);
    // Cache is filter-aware: a different filter re-uploads in place so all
    // existing pointers keep working with the new sampling.
    if (it != textures_.end()) {
        if (it->second.filter != filter) {
            Texture2D tmp;
            if (tmp.loadFile(path, true, filter)) {
                *it->second.tex = std::move(tmp);
                it->second.filter = filter;
                it->second.mtime = fileMtime(path);
            }
        }
        return it->second.tex.get();
    }
    std::string resolved = resolve(path);
    auto tex = std::make_unique<Texture2D>();
    if (!tex->loadFile(resolved, true, filter)) return nullptr;
    Texture2D* ptr = tex.get();
    textures_[path] = TexEntry{std::move(tex), fileMtime(resolved), filter};
    FORGE_LOG_DEBUG("Asset: texture '%s' (%dx%d)", resolved.c_str(), ptr->width(), ptr->height());
    return ptr;
}

Font* AssetManager::loadFont(const std::string& path, float pxHeight) {
    // Float-safe key: hash the bit pattern, not a truncated int cast.
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(pxHeight), "float size");
    std::memcpy(&bits, &pxHeight, sizeof(bits));
    std::string key = path + "#" + std::to_string(bits);
    auto it = fonts_.find(key);
    if (it != fonts_.end()) return it->second.font.get();
    std::string resolved = resolve(path);
    auto f = std::make_unique<Font>();
    if (!f->loadFile(resolved, pxHeight)) return nullptr;
    Font* ptr = f.get();
    fonts_[key] = FontEntry{std::move(f), fileMtime(resolved), pxHeight};
    return ptr;
}

SoundHandle AssetManager::loadSound(const std::string& path) {
    auto it = sounds_.find(path);
    if (it != sounds_.end()) return it->second.handle;
    if (!audio_) {
        FORGE_LOG_ERROR("Asset: no AudioEngine for '%s'", path.c_str());
        return {};
    }
    std::string resolved = resolve(path);
    SoundHandle h = audio_->loadFile(resolved);
    if (h.valid()) sounds_[path] = SoundEntry{h, fileMtime(resolved)};
    return h;
}

void AssetManager::unloadTexture(const std::string& path) {
    textures_.erase(path);
}

Shader* AssetManager::loadShader(const std::string& vertPath, const std::string& fragPath) {
    std::string key = vertPath + "|" + fragPath;
    auto it = shaders_.find(key);
    if (it != shaders_.end()) return it->second.shader.get();
    std::string v = resolve(vertPath), f = resolve(fragPath);
    auto shader = std::make_unique<Shader>();
    if (!shader->loadFiles(v, f)) {
        FORGE_LOG_ERROR("Asset: shader '%s' + '%s' failed", v.c_str(), f.c_str());
        return nullptr;
    }
    Shader* ptr = shader.get();
    shaders_[key] = ShaderEntry{std::move(shader), v, f, fileMtime(v), fileMtime(f)};
    return ptr;
}

void AssetManager::clear() {
    textures_.clear();
    fonts_.clear();
    shaders_.clear();
    if (audio_) {
        for (auto& kv : sounds_) audio_->unload(kv.second.handle);
    }
    sounds_.clear();
}

int AssetManager::hotReload() {
    int n = 0;
    for (auto& kv : textures_) {
        std::string path = resolve(kv.first);
        int64_t mt = fileMtime(path);
        if (mt > kv.second.mtime) {
            Texture2D tmp;
            if (tmp.loadFile(path, true, kv.second.filter)) {
                *kv.second.tex = std::move(tmp);
                kv.second.mtime = mt;
                ++n;
                FORGE_LOG_INFO("Hot-reload: '%s'", kv.first.c_str());
            }
        }
    }
    for (auto& kv : fonts_) {
        std::string path = resolve(kv.first.substr(0, kv.first.find('#')));
        int64_t mt = fileMtime(path);
        if (mt > kv.second.mtime) {
            Font tmp;
            if (tmp.loadFile(path, kv.second.px)) {
                *kv.second.font = std::move(tmp);
                kv.second.mtime = mt;
                ++n;
            }
        }
    }
    if (audio_) {
        for (auto& kv : sounds_) {
            int64_t mt = fileMtime(resolve(kv.first));
            if (mt > kv.second.mtime) {
                if (audio_->reloadFile(kv.second.handle, resolve(kv.first))) {
                    kv.second.mtime = mt;
                    ++n;
                    FORGE_LOG_INFO("Hot-reload: '%s'", kv.first.c_str());
                }
            }
        }
    }
    // Shaders: reload only when a source file changed, and only swap on a
    // successful compile so a typo never blanks the running game.
    for (auto& kv : shaders_) {
        int64_t vm = fileMtime(kv.second.vertPath);
        int64_t fm = fileMtime(kv.second.fragPath);
        if (vm > kv.second.vertMtime || fm > kv.second.fragMtime) {
            Shader tmp;
            if (tmp.loadFiles(kv.second.vertPath, kv.second.fragPath)) {
                *kv.second.shader = std::move(tmp);
                kv.second.vertMtime = vm;
                kv.second.fragMtime = fm;
                ++n;
                FORGE_LOG_INFO("Hot-reload: shader '%s'", kv.first.c_str());
            }
        }
    }
    return n;
}

void AssetManager::update() {
    if (!hotReload_) return;
    // Throttle: stat() every asset at most twice per second, not every frame.
    auto now = std::chrono::steady_clock::now();
    if (now - lastCheck_ < std::chrono::milliseconds(500)) return;
    lastCheck_ = now;
    hotReload();
}

} // namespace forge
