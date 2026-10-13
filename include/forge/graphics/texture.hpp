#pragma once
// Texture2D: GPU texture + CPU pixels. Loads PNG/JPG/BMP/TGA via stb_image.
// White 1x1 fallback keeps the batch alive when assets are missing.

#include "forge/math/math.hpp"
#include <string>
#include <cstdint>

namespace forge {

enum class TextureFilter { Nearest, Linear };
enum class TextureWrap { Clamp, Repeat };

class Texture2D {
public:
    Texture2D() = default;
    ~Texture2D();

    Texture2D(const Texture2D&) = delete;
    Texture2D& operator=(const Texture2D&) = delete;
    Texture2D(Texture2D&& o) noexcept;
    Texture2D& operator=(Texture2D&& o) noexcept;

    bool loadFile(const std::string& path, bool flipY = true,
                  TextureFilter filter = TextureFilter::Linear,
                  TextureWrap wrap = TextureWrap::Clamp);
    bool loadMemory(const uint8_t* data, int w, int h, int channels,
                    TextureFilter filter = TextureFilter::Linear,
                    TextureWrap wrap = TextureWrap::Clamp);
    // Procedural solid / checker / circle for prototyping without assets
    bool createSolid(Color c, TextureFilter filter = TextureFilter::Nearest);
    bool createChecker(int cells, Color a, Color b, int cellPx = 8);

    void destroy();
    bool valid() const { return id_ != 0; }
    void bind(int slot = 0) const;

    int width() const { return w_; }
    int height() const { return h_; }
    Vec2 size() const { return {(float)w_, (float)h_}; }
    uint32_t id() const { return id_; }
    const std::string& path() const { return path_; }

    static Texture2D& white(); // shared 1x1 white (must be called after GL init)

private:
    void upload(const uint8_t* pixels, int w, int h, int channels,
                TextureFilter filter, TextureWrap wrap);
    uint32_t id_ = 0;
    int w_ = 0, h_ = 0, channels_ = 4;
    std::string path_;
};

} // namespace forge
