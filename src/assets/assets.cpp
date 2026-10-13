#include "forge/assets/assets.hpp"
#include "forge/core/logger.hpp"
#include <sys/stat.h>

namespace forge {

AssetManager::AssetManager(AudioEngine* audio) : audio_(audio) {}

int64_t AssetManager::fileMtime(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return 0;
    return (int64_t)st.st_mtime;
}

Texture2D* AssetManager::loadTexture(const std::string& path, TextureFilter filter) {
    auto it = textures_.find(path);
    if (it != textures_.end()) return it->second.tex.get();
    auto tex = std::make_unique<Texture2D>();
    if (!tex->loadFile(path, true, filter)) return nullptr;
    Texture2D* ptr = tex.get();
    textures_[path] = TexEntry{std::move(tex), fileMtime(path), filter};
    FORGE_LOG_DEBUG("Asset: texture '%s' (%dx%d)", path.c_str(), ptr->width(), ptr->height());
    return ptr;
}

Font* AssetManager::loadFont(const std::string& path, float pxHeight) {
    std::string key = path + "#" + std::to_string((int)pxHeight);
    auto it = fonts_.find(key);
    if (it != fonts_.end()) return it->second.font.get();
    auto f = std::make_unique<Font>();
    if (!f->loadFile(path, pxHeight)) return nullptr;
    Font* ptr = f.get();
    fonts_[key] = FontEntry{std::move(f), fileMtime(path), pxHeight};
    return ptr;
}

SoundHandle AssetManager::loadSound(const std::string& path) {
    auto it = sounds_.find(path);
    if (it != sounds_.end()) return it->second;
    if (!audio_) {
        FORGE_LOG_ERROR("Asset: no AudioEngine for '%s'", path.c_str());
        return {};
    }
    SoundHandle h = audio_->loadFile(path);
    if (h.valid()) sounds_[path] = h;
    return h;
}

void AssetManager::unloadTexture(const std::string& path) {
    textures_.erase(path);
}

void AssetManager::clear() {
    textures_.clear();
    fonts_.clear();
    if (audio_) {
        for (auto& kv : sounds_) audio_->unload(kv.second);
    }
    sounds_.clear();
}

int AssetManager::hotReload() {
    int n = 0;
    for (auto& kv : textures_) {
        int64_t mt = fileMtime(kv.first);
        if (mt > kv.second.mtime) {
            Texture2D tmp;
            if (tmp.loadFile(kv.first, true, kv.second.filter)) {
                *kv.second.tex = std::move(tmp);
                kv.second.mtime = mt;
                ++n;
                FORGE_LOG_INFO("Hot-reload: '%s'", kv.first.c_str());
            }
        }
    }
    for (auto& kv : fonts_) {
        std::string path = kv.first.substr(0, kv.first.find('#'));
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
    return n;
}

void AssetManager::update() {
    if (hotReload_) hotReload();
}

} // namespace forge
