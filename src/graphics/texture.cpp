#include "forge/graphics/texture.hpp"
#include "forge/graphics/gl_loader.hpp"
#include "forge/core/logger.hpp"
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace forge {

Texture2D::~Texture2D() { destroy(); }

Texture2D::Texture2D(Texture2D&& o) noexcept
    : id_(o.id_), w_(o.w_), h_(o.h_), channels_(o.channels_), path_(std::move(o.path_)) {
    o.id_ = 0; o.w_ = o.h_ = 0;
}
Texture2D& Texture2D::operator=(Texture2D&& o) noexcept {
    if (this != &o) {
        destroy();
        id_ = o.id_; w_ = o.w_; h_ = o.h_; channels_ = o.channels_; path_ = std::move(o.path_);
        o.id_ = 0; o.w_ = o.h_ = 0;
    }
    return *this;
}

void Texture2D::upload(const uint8_t* pixels, int w, int h, int channels,
                       TextureFilter filter, TextureWrap wrap) {
    destroy();
    w_ = w; h_ = h; channels_ = channels;
    gl::glGenTextures(1, &id_);
    gl::glBindTexture(GL_TEXTURE_2D, id_);
    gl::glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    GLenum fmt = channels == 4 ? GL_RGBA : (channels == 3 ? GL_RGB : GL_RED);
    gl::glTexImage2D(GL_TEXTURE_2D, 0, (GLint)fmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, pixels);
    int minF = filter == TextureFilter::Nearest ? GL_NEAREST : GL_LINEAR;
    int magF = minF;
    gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minF);
    gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magF);
    int wmode = GL_CLAMP_TO_EDGE;
    if (wrap == TextureWrap::Repeat) wmode = GL_REPEAT;
    gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wmode);
    gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wmode);
    gl::glBindTexture(GL_TEXTURE_2D, 0);
}

bool Texture2D::loadFile(const std::string& path, bool flipY,
                         TextureFilter filter, TextureWrap wrap) {
    stbi_set_flip_vertically_on_load(flipY ? 1 : 0);
    int w, h, ch;
    uint8_t* px = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!px) {
        FORGE_LOG_ERROR("Texture load failed '%s': %s", path.c_str(), stbi_failure_reason());
        return false;
    }
    upload(px, w, h, 4, filter, wrap);
    stbi_image_free(px);
    path_ = path;
    return true;
}

bool Texture2D::loadMemory(const uint8_t* data, int w, int h, int channels,
                           TextureFilter filter, TextureWrap wrap) {
    if (!data || w <= 0 || h <= 0) return false;
    upload(data, w, h, channels, filter, wrap);
    return true;
}

bool Texture2D::createSolid(Color c, TextureFilter filter) {
    uint8_t px[4] = {
        (uint8_t)(clamp01(c.r) * 255), (uint8_t)(clamp01(c.g) * 255),
        (uint8_t)(clamp01(c.b) * 255), (uint8_t)(clamp01(c.a) * 255)};
    upload(px, 1, 1, 4, filter, TextureWrap::Clamp);
    return true;
}

bool Texture2D::createChecker(int cells, Color a, Color b, int cellPx) {
    int size = cells * cellPx;
    std::vector<uint8_t> px((size_t)size * size * 4);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            bool even = ((x / cellPx) + (y / cellPx)) % 2 == 0;
            Color c = even ? a : b;
            size_t i = ((size_t)y * size + x) * 4;
            px[i+0] = (uint8_t)(c.r * 255); px[i+1] = (uint8_t)(c.g * 255);
            px[i+2] = (uint8_t)(c.b * 255); px[i+3] = 255;
        }
    upload(px.data(), size, size, 4, TextureFilter::Nearest, TextureWrap::Repeat);
    return true;
}

void Texture2D::destroy() {
    if (id_) {
        if (gl::isLoaded()) gl::glDeleteTextures(1, &id_);
        id_ = 0;
    }
    w_ = h_ = 0;
    path_.clear();
}

void Texture2D::bind(int slot) const {
    gl::glActiveTexture((GLenum)(GL_TEXTURE0 + slot));
    gl::glBindTexture(GL_TEXTURE_2D, id_);
}

Texture2D& Texture2D::white() {
    static Texture2D w;
    if (!w.valid() && gl::isLoaded()) {
        uint8_t px[4] = {255, 255, 255, 255};
        w.upload(px, 1, 1, 4, TextureFilter::Nearest, TextureWrap::Clamp);
    }
    return w;
}

} // namespace forge
