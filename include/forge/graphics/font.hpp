#pragma once
// Font: stb_truetype, dynamic multi-page atlas, UTF-8, word wrap.
//
// Glyphs are baked on demand (not just ASCII), packed into 1024x1024 RGBA
// pages with a shelf allocator, and uploaded incrementally. Pages are created
// lazily, so a font with only Latin text still uses a single page.
//
// `draw`/`measure` are const but may bake new glyphs (the atlas is mutable
// cache state). All coordinates are in the current batch's space; `pos` is the
// TOP-LEFT of the text block (Y-up world/screen), matching the 0.1.0 API.

#include "forge/math/math.hpp"
#include "forge/graphics/texture.hpp"
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>

namespace forge {

class SpriteBatch;

struct GlyphInfo {
    Rect uv;         // pixel rect within atlas page (top-left origin)
    Vec2 offset;     // bearing offset from the baseline pen (stb, Y-down)
    Vec2 size;       // glyph bitmap size in px
    float advance = 0;
    int page = 0;    // atlas page index
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
    // Pre-baked bitmap font: BMFont text format (`.fnt`) + one or more PNG
    // atlas pages next to it. Glyph metrics/UVs come from the file; no
    // stb_truetype, no baking, and drawing is a plain atlas lookup. Returns
    // false on a malformed file or a missing page image.
    bool loadBitmapFnt(const std::string& fntPath);
    bool bitmap() const { return bitmap_; }
    void destroy();
    bool valid() const { return !pages_.empty(); }

    float height() const { return pixelHeight_; }
    // TTF: pixel height * 1.25. BMFont: the file's own lineHeight.
    float lineHeight() const { return bitmap_ ? pixelHeight_ : pixelHeight_ * 1.25f; }

    // --- UTF-8 helpers (public: useful for wrapping/layout code) ---
    // Decodes UTF-8 into codepoints; invalid sequences become U+FFFD.
    static std::vector<uint32_t> decodeUtf8(std::string_view text);
    // Encodes one codepoint to UTF-8 (appends).
    static void encodeUtf8(uint32_t cp, std::string& out);

    // --- metrics ---
    Vec2 measure(const std::string& text, float scale = 1.0f) const;
    float widthOf(const std::string& text, float scale = 1.0f) const;
    // Height of `text` wrapped into `maxWidth` (world units, scale applied).
    float heightWrapped(const std::string& text, float maxWidth, float scale = 1.0f) const;
    // Width of the widest wrapped line.
    float widthWrapped(const std::string& text, float maxWidth, float scale = 1.0f) const;

    // --- drawing ---
    // Draw with top-left origin at pos (screen/world space of current batch).
    void draw(SpriteBatch& batch, const std::string& text, Vec2 pos,
              float scale, Color c) const;
    void drawCentered(SpriteBatch& batch, const std::string& text, Vec2 center,
                      float scale, Color c) const;
    // Word-wrapped draw. Long words are broken by codepoint. `maxWidth` is in
    // the same units as pos/size (before scale is applied to glyph metrics).
    void drawWrapped(SpriteBatch& batch, const std::string& text, Vec2 pos,
                     float maxWidth, float scale, Color c) const;
    // Like drawWrapped, but pos is the top-center of the block.
    void drawWrappedCentered(SpriteBatch& batch, const std::string& text, Vec2 topCenter,
                             float maxWidth, float scale, Color c) const;

    const Texture2D& atlas(int page = 0) const;
    int pageCount() const { return (int)pages_.size(); }
    bool hasGlyph(uint32_t codepoint) const;

    // Codepoints are baked on demand; this is the atlas page size.
    static constexpr int kAtlasPage = 1024;

private:
    struct Page {
        Texture2D tex;
        std::vector<uint8_t> rgba;
        int cursorX = 1, cursorY = 1, rowH = 0;
        int w = 0, h = 0;
        bool created = false;
    };

    bool bakeMetrics(const uint8_t* ttf, float px);
    // Returns the glyph, baking it into a page if needed (mutable cache).
    const GlyphInfo* glyphFor(uint32_t cp) const;
    // Splits text into wrapped lines (UTF-8), honoring explicit '\n'.
    std::vector<std::string> wrapLines(const std::string& text, float maxWidth,
                                       float scale) const;
    float advanceOf(uint32_t cp, float scale) const;
    void ensurePage(Page& p) const;

    // stbtt_fontinfo is stored by value; its `data` pointer aliases ttfCopy_.
    struct Impl;
    Impl* impl_ = nullptr;

    mutable std::vector<Page> pages_;
    mutable std::unordered_map<uint32_t, GlyphInfo> glyphs_;
    mutable bool missingLogged_ = false;
    bool bitmap_ = false;

    float pixelHeight_ = 32.0f;
    float ascent_ = 0.0f, descent_ = 0.0f, lineGap_ = 0.0f;
    uint8_t* ttfCopy_ = nullptr;
    size_t ttfLen_ = 0;
};

} // namespace forge
