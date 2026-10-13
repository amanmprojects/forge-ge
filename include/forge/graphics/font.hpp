#pragma once
// Font: stb_truetype baked atlas, batched through SpriteBatch.
// One texture per font; ASCII 32..126 cached + dynamic fallback.

#include "forge/math/math.hpp"
#include "forge/graphics/texture.hpp"
#include <string>

namespace forge {

class SpriteBatch;

struct GlyphInfo {
    Rect uv;      // in pixels within atlas
    Vec2 offset;  // bearing offset
    Vec2 size;    // glyph size px
    float advance = 0;
};

class Font {
public:
    Font() = default;
    ~Font();

    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;
    Font(Font&& o) noexcept;
    Font& operator=(Font&& o) noexcept;

    bool loadFile(const std::string& path, float pixelHeight = 32.0f);
    bool loadMemory(const uint8_t* ttfData, size_t len, float pixelHeight = 32.0f);
    void destroy();
    bool valid() const { return atlas_.valid(); }

    float height() const { return pixelHeight_; }
    float lineHeight() const { return pixelHeight_ * 1.25f; }

    Vec2 measure(const std::string& text, float scale = 1.0f) const;
    float widthOf(const std::string& text, float scale = 1.0f) const;

    // Draw with top-left origin at pos (screen/world space of current batch)
    void draw(SpriteBatch& batch, const std::string& text, Vec2 pos,
              float scale, Color c) const;
    void drawCentered(SpriteBatch& batch, const std::string& text, Vec2 center,
                      float scale, Color c) const;

    const Texture2D& atlas() const { return atlas_; }

private:
    bool bake(const uint8_t* ttf, float px);
    const GlyphInfo* glyph(char ch) const;

    Texture2D atlas_;
    GlyphInfo glyphs_[96] = {};
    float pixelHeight_ = 32.0f;
    float ascent_ = 0.0f, descent_ = 0.0f;
    uint8_t* ttfCopy_ = nullptr;
    size_t ttfLen_ = 0;
};

} // namespace forge
