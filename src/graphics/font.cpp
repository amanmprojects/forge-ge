#include "forge/graphics/font.hpp"
#include "forge/graphics/sprite_batch.hpp"
#include "forge/core/logger.hpp"
#include <cstring>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

namespace forge {

Font::~Font() { destroy(); }

Font::Font(Font&& o) noexcept
    : atlas_(std::move(o.atlas_)), pixelHeight_(o.pixelHeight_),
      ascent_(o.ascent_), descent_(o.descent_),
      ttfCopy_(o.ttfCopy_), ttfLen_(o.ttfLen_) {
    std::memcpy(glyphs_, o.glyphs_, sizeof(glyphs_));
    o.ttfCopy_ = nullptr; o.ttfLen_ = 0;
}

Font& Font::operator=(Font&& o) noexcept {
    if (this != &o) {
        destroy();
        atlas_ = std::move(o.atlas_);
        std::memcpy(glyphs_, o.glyphs_, sizeof(glyphs_));
        pixelHeight_ = o.pixelHeight_; ascent_ = o.ascent_; descent_ = o.descent_;
        ttfCopy_ = o.ttfCopy_; ttfLen_ = o.ttfLen_;
        o.ttfCopy_ = nullptr; o.ttfLen_ = 0;
    }
    return *this;
}

bool Font::loadFile(const std::string& path, float pixelHeight) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        FORGE_LOG_ERROR("Font file not found: %s", path.c_str());
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf((size_t)len);
    if (std::fread(buf.data(), 1, (size_t)len, f) != (size_t)len) {
        std::fclose(f);
        return false;
    }
    std::fclose(f);
    return loadMemory(buf.data(), buf.size(), pixelHeight);
}

bool Font::loadMemory(const uint8_t* ttfData, size_t len, float pixelHeight) {
    destroy();
    if (!ttfData || len == 0) return false;
    ttfCopy_ = new uint8_t[len];
    std::memcpy(ttfCopy_, ttfData, len);
    ttfLen_ = len;
    pixelHeight_ = pixelHeight;
    if (!bake(ttfCopy_, pixelHeight)) {
        delete[] ttfCopy_; ttfCopy_ = nullptr; ttfLen_ = 0;
        return false;
    }
    return true;
}

bool Font::bake(const uint8_t* ttf, float px) {
    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, ttf, 0)) {
        FORGE_LOG_ERROR("stbtt_InitFont failed (not a TTF?)");
        return false;
    }
    float scale = stbtt_ScaleForPixelHeight(&info, px);
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);
    ascent_ = ascent * scale;
    descent_ = descent * scale;

    // Atlas: 512x512 single channel, ASCII 32..126
    const int ATLAS = 512;
    std::vector<uint8_t> alpha((size_t)ATLAS * ATLAS, 0);
    int x = 2, y = 2, rowH = 0;

    for (int ch = 32; ch < 127; ++ch) {
        int gw, gh, xoff, yoff;
        uint8_t* bmp = stbtt_GetCodepointBitmap(&info, scale, scale, ch, &gw, &gh, &xoff, &yoff);
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&info, ch, &adv, &lsb);

        if (x + gw + 2 > ATLAS) { x = 2; y += rowH + 2; rowH = 0; }
        if (y + gh + 2 > ATLAS) {
            FORGE_LOG_ERROR("Font atlas overflow at %dpx (char %d)", (int)px, ch);
            if (bmp) stbtt_FreeBitmap(bmp, nullptr);
            return false;
        }
        if (bmp && gw > 0 && gh > 0) {
            for (int r = 0; r < gh; ++r)
                std::memcpy(&alpha[(size_t)(y + r) * ATLAS + x], bmp + (size_t)r * gw, (size_t)gw);
            stbtt_FreeBitmap(bmp, nullptr);
        }
        GlyphInfo& g = glyphs_[ch - 32];
        g.uv = {(float)x, (float)y, (float)gw, (float)gh};
        g.offset = {(float)xoff, (float)yoff};
        g.size = {(float)gw, (float)gh};
        g.advance = adv * scale;

        x += gw + 2;
        if (gh + 2 > rowH) rowH = gh + 2;
    }

    // Flip vertically so atlas layout matches flipY=true textures:
    // packed-top-down (x,y) -> memory rows [H-y-h, H-y), and the existing
    // UV math ({x/W, 1-(y+h)/H, ...}) samples it upright.
    for (int r = 0; r < ATLAS / 2; ++r) {
        uint8_t* a = &alpha[(size_t)r * ATLAS];
        uint8_t* b = &alpha[(size_t)(ATLAS - 1 - r) * ATLAS];
        for (int c = 0; c < ATLAS; ++c) std::swap(a[c], b[c]);
    }

    // Expand to RGBA
    std::vector<uint8_t> rgba((size_t)ATLAS * ATLAS * 4);
    for (size_t i = 0; i < (size_t)ATLAS * ATLAS; ++i) {
        rgba[i*4+0] = 255; rgba[i*4+1] = 255; rgba[i*4+2] = 255;
        rgba[i*4+3] = alpha[i];
    }
    atlas_.loadMemory(rgba.data(), ATLAS, ATLAS, 4, TextureFilter::Linear, TextureWrap::Clamp);
    return atlas_.valid();
}

void Font::destroy() {
    atlas_.destroy();
    std::memset(glyphs_, 0, sizeof(glyphs_));
    delete[] ttfCopy_;
    ttfCopy_ = nullptr; ttfLen_ = 0;
}

const GlyphInfo* Font::glyph(char ch) const {
    if (ch < 32 || ch >= 127) ch = '?';
    return &glyphs_[ch - 32];
}

Vec2 Font::measure(const std::string& text, float scale) const {
    float w = 0, maxW = 0, h = lineHeight() * scale;
    int lines = 1;
    for (char ch : text) {
        if (ch == '\n') { maxW = std::max(maxW, w); w = 0; ++lines; continue; }
        w += glyph(ch)->advance * scale;
    }
    maxW = std::max(maxW, w);
    return {maxW, h * lines};
}

float Font::widthOf(const std::string& text, float scale) const {
    return measure(text, scale).x;
}

void Font::draw(SpriteBatch& batch, const std::string& text, Vec2 pos,
                float scale, Color c) const {
    if (!valid()) return;
    float penX = pos.x;
    // pos is the TOP-LEFT of the text block (Y-up batch space).
    float baseline = pos.y - ascent_ * scale;
    float penY = baseline;
    float lh = lineHeight() * scale;
    float atlW = (float)atlas_.width(), atlH = (float)atlas_.height();

    for (char ch : text) {
        if (ch == '\n') { penX = pos.x; penY -= lh; baseline = penY; continue; }
        if (ch == ' ') { penX += glyph(' ')->advance * scale; continue; }
        const GlyphInfo* g = glyph(ch);
        if (g->size.x > 0 && g->size.y > 0) {
            // stb xoff/yoff are Y-down offsets from the baseline pen:
            // bitmap top-left = (penX + xoff, baseline + yoff) in Y-down.
            // Convert to Y-up: top = baseline - yoff, center = top - h/2.
            Vec2 gp{
                penX + g->offset.x * scale + g->size.x * scale * 0.5f,
                penY - g->offset.y * scale - g->size.y * scale * 0.5f
            };
            SpriteDraw d;
            d.tex = &atlas_;
            d.pos = gp;
            d.size = {g->size.x * scale, g->size.y * scale};
            d.color = c;
            // atlas is top-left origin; flip Y into GL UV space
            d.src = {g->uv.x / atlW, 1.0f - (g->uv.y + g->uv.h) / atlH,
                     g->uv.w / atlW, g->uv.h / atlH};
            batch.draw(d);
        }
        penX += g->advance * scale;
    }
}

void Font::drawCentered(SpriteBatch& batch, const std::string& text, Vec2 center,
                        float scale, Color c) const {
    Vec2 m = measure(text, scale);
    draw(batch, text, {center.x - m.x * 0.5f, center.y + m.y * 0.5f}, scale, c);
}

} // namespace forge
