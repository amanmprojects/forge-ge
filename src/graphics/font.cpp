#include "forge/graphics/font.hpp"
#include "forge/graphics/sprite_batch.hpp"
#include "forge/core/logger.hpp"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <cmath>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

namespace forge {

struct Font::Impl {
    stbtt_fontinfo info{};
    bool ready = false;
    float scale = 1.0f;
};

// ------------------------------------------------------------------ UTF-8
std::vector<uint32_t> Font::decodeUtf8(std::string_view text) {
    std::vector<uint32_t> out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        unsigned char c = (unsigned char)text[i];
        uint32_t cp = 0;
        int extra = 0;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; extra = 3; }
        else { out.push_back(0xFFFD); ++i; continue; }

        if (i + (size_t)extra >= text.size()) { out.push_back(0xFFFD); break; }
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            unsigned char cc = (unsigned char)text[i + (size_t)k];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (!ok) { out.push_back(0xFFFD); ++i; continue; }
        // Reject overlong/surrogate/out-of-range sequences.
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
            (extra == 3 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            out.push_back(0xFFFD);
        } else {
            out.push_back(cp);
        }
        i += (size_t)extra + 1;
    }
    return out;
}

void Font::encodeUtf8(uint32_t cp, std::string& out) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) {
        out.push_back((char)cp);
    } else if (cp < 0x800) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

// ---------------------------------------------------------------- lifecycle
Font::~Font() { destroy(); }

Font::Font(Font&& o) noexcept
    : impl_(o.impl_), pages_(std::move(o.pages_)), glyphs_(std::move(o.glyphs_)),
      missingLogged_(o.missingLogged_), pixelHeight_(o.pixelHeight_),
      ascent_(o.ascent_), descent_(o.descent_), lineGap_(o.lineGap_),
      ttfCopy_(o.ttfCopy_), ttfLen_(o.ttfLen_) {
    o.impl_ = nullptr;
    o.ttfCopy_ = nullptr; o.ttfLen_ = 0;
}

Font& Font::operator=(Font&& o) noexcept {
    if (this != &o) {
        destroy();
        impl_ = o.impl_;
        pages_ = std::move(o.pages_);
        glyphs_ = std::move(o.glyphs_);
        missingLogged_ = o.missingLogged_;
        pixelHeight_ = o.pixelHeight_;
        ascent_ = o.ascent_; descent_ = o.descent_; lineGap_ = o.lineGap_;
        ttfCopy_ = o.ttfCopy_; ttfLen_ = o.ttfLen_;
        o.impl_ = nullptr;
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
    if (len <= 0) { std::fclose(f); return false; }
    std::vector<uint8_t> buf((size_t)len);
    size_t got = std::fread(buf.data(), 1, (size_t)len, f);
    std::fclose(f);
    if (got != (size_t)len) return false;
    return loadMemory(buf.data(), buf.size(), pixelHeight);
}

bool Font::loadMemory(const uint8_t* ttfData, size_t len, float pixelHeight) {
    destroy();
    if (!ttfData || len == 0) return false;
    ttfCopy_ = new uint8_t[len];
    std::memcpy(ttfCopy_, ttfData, len);
    ttfLen_ = len;
    pixelHeight_ = pixelHeight;
    if (!bakeMetrics(ttfCopy_, pixelHeight)) {
        delete[] ttfCopy_; ttfCopy_ = nullptr; ttfLen_ = 0;
        return false;
    }
    // Page 0 is created eagerly so valid() is true and callers can bind it.
    pages_.emplace_back();
    ensurePage(pages_[0]);
    return valid();
}

bool Font::loadBitmapFnt(const std::string& fntPath) {
    std::FILE* f = std::fopen(fntPath.c_str(), "rb");
    if (!f) {
        FORGE_LOG_ERROR("Font: cannot open '%s'", fntPath.c_str());
        return false;
    }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);

    destroy();
    bitmap_ = true;
    pixelHeight_ = 16.0f;

    // Base directory for page images (same folder as the .fnt).
    std::string dir;
    size_t slash = fntPath.find_last_of("/\\");
    if (slash != std::string::npos) dir = fntPath.substr(0, slash + 1);

    // BMFont text: "key=value" pairs, `page` lines then `char` lines.
    std::vector<std::string> pageFiles;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? text.size() : end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;

        auto number = [&](const char* key, float def) -> float {
            size_t k = line.find(key);
            if (k == std::string::npos) return def;
            k += std::strlen(key);
            return (float)std::atof(line.c_str() + k);
        };
        if (line.rfind("common ", 0) == 0) {
            float lh = number("lineHeight=", 0);
            if (lh > 0) pixelHeight_ = lh;
        } else if (line.rfind("page ", 0) == 0) {
            size_t k = line.find("file=");
            if (k != std::string::npos) {
                std::string name = line.substr(k + 5);
                if (!name.empty() && name.front() == '"' && name.back() == '"')
                    name = name.substr(1, name.size() - 2);
                pageFiles.push_back(name);
            }
        } else if (line.rfind("char ", 0) == 0) {
            uint32_t id = (uint32_t)number("id=", 0);
            GlyphInfo g;
            float x = number("x=", 0), y = number("y=", 0);
            float w = number("width=", 0), h = number("height=", 0);
            g.uv = {x, y, w, h};
            g.size = {w, h};
            g.offset = {number("xoffset=", 0), number("yoffset=", 0)};
            g.advance = number("xadvance=", w);
            g.page = (int)number("page=", 0);
            glyphs_[id] = g;
        }
    }

    if (glyphs_.empty() || pageFiles.empty()) {
        FORGE_LOG_ERROR("Font '%s': no glyphs or pages (BMFont text format required)",
                        fntPath.c_str());
        destroy();
        return false;
    }
    for (auto& name : pageFiles) {
        pages_.emplace_back();
        if (!pages_.back().tex.loadFile(dir + name, false, TextureFilter::Nearest)) {
            FORGE_LOG_ERROR("Font '%s': missing atlas page '%s'", fntPath.c_str(),
                            (dir + name).c_str());
            destroy();
            return false;
        }
        pages_.back().created = true;
    }
    return valid();
}

bool Font::bakeMetrics(const uint8_t* ttf, float px) {
    impl_ = new Impl{};
    if (!stbtt_InitFont(&impl_->info, ttf, 0)) {
        FORGE_LOG_ERROR("stbtt_InitFont failed (not a TTF?)");
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    impl_->ready = true;
    impl_->scale = stbtt_ScaleForPixelHeight(&impl_->info, px);
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&impl_->info, &ascent, &descent, &lineGap);
    ascent_ = ascent * impl_->scale;
    descent_ = descent * impl_->scale;
    lineGap_ = lineGap * impl_->scale;
    glyphs_.clear();
    glyphs_.reserve(256);
    return true;
}

void Font::destroy() {
    pages_.clear();
    glyphs_.clear();
    bitmap_ = false;
    delete impl_;
    impl_ = nullptr;
    delete[] ttfCopy_;
    ttfCopy_ = nullptr;
    ttfLen_ = 0;
    missingLogged_ = false;
}

const Texture2D& Font::atlas(int page) const {
    static Texture2D empty;
    if (page < 0 || page >= (int)pages_.size()) return empty;
    return pages_[(size_t)page].tex;
}

void Font::ensurePage(Page& p) const {
    if (p.created) return;
    p.w = p.h = kAtlasPage;
    p.rgba.assign((size_t)p.w * p.h * 4, 0);
    p.tex.loadMemory(p.rgba.data(), p.w, p.h, 4, TextureFilter::Linear,
                     TextureWrap::Clamp);
    p.created = true;
    p.cursorX = p.cursorY = 1;
    p.rowH = 0;
}

// ---------------------------------------------------------------- glyph cache
bool Font::hasGlyph(uint32_t cp) const {
    if (!impl_ || !impl_->ready) return false;
    return stbtt_FindGlyphIndex(&impl_->info, (int)cp) != 0;
}

const GlyphInfo* Font::glyphFor(uint32_t cp) const {
    if (bitmap_) {
        auto it = glyphs_.find(cp);
        if (it != glyphs_.end()) return &it->second;
        auto q = glyphs_.find('?');
        if (q != glyphs_.end()) return &q->second;
        static GlyphInfo blank;
        return &blank;
    }
    if (!impl_ || !impl_->ready) return nullptr;
    auto it = glyphs_.find(cp);
    if (it != glyphs_.end()) return &it->second;

    // Missing glyph: fall back to '?' once, then to a blank advance so layout
    // stays stable instead of recursing forever.
    int gidx = stbtt_FindGlyphIndex(&impl_->info, (int)cp);
    if (gidx == 0 && cp != '?') {
        if (!missingLogged_) {
            FORGE_LOG_WARN("Font: glyph U+%04X missing, using '?'", cp);
            missingLogged_ = true;
        }
        return glyphFor('?');
    }

    int gw = 0, gh = 0, xoff = 0, yoff = 0;
    float scale = impl_->scale;
    uint8_t* bmp = stbtt_GetCodepointBitmap(&impl_->info, scale, scale, (int)cp,
                                            &gw, &gh, &xoff, &yoff);
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&impl_->info, (int)cp, &adv, &lsb);

    GlyphInfo g;
    g.offset = {(float)xoff, (float)yoff};
    g.size = {(float)gw, (float)gh};
    g.advance = adv * scale;

    if (gw > 0 && gh > 0) {
        // Shelf-pack: use the first page whose current shelf fits, or which
        // has room for one more shelf. Otherwise open a new page.
        int pageIdx = -1;
        for (int i = 0; i < (int)pages_.size(); ++i) {
            Page& p = pages_[(size_t)i];
            ensurePage(p);
            bool fitsShelf = p.cursorX + gw + 1 <= p.w &&
                             p.cursorY + std::max(gh, p.rowH) + 1 <= p.h;
            bool fitsNewShelf = gw + 2 <= p.w &&
                                p.cursorY + p.rowH + 1 + gh + 1 <= p.h;
            if (fitsShelf || fitsNewShelf) { pageIdx = i; break; }
        }
        if (pageIdx < 0) {
            // A glyph larger than a page cannot be packed; drop it rather than
            // loop forever (it still advances, so layout stays stable).
            if (gw + 2 > kAtlasPage || gh + 2 > kAtlasPage) {
                FORGE_LOG_WARN("Font: glyph U+%04X (%dx%d px) exceeds atlas page",
                               cp, gw, gh);
                gw = gh = 0;
            } else {
                pages_.emplace_back();
                pageIdx = (int)pages_.size() - 1;
                ensurePage(pages_[(size_t)pageIdx]);
            }
        }
        if (pageIdx >= 0 && gw > 0 && gh > 0) {
            Page& p = pages_[(size_t)pageIdx];
            if (p.cursorX + gw + 1 > p.w) { // next shelf
                p.cursorX = 1;
                p.cursorY += p.rowH + 1;
                p.rowH = 0;
            }
            int px = p.cursorX, py = p.cursorY;
            std::vector<uint8_t> tight((size_t)gw * (size_t)gh * 4);
            for (int r = 0; r < gh; ++r) {
                uint8_t* dst = &p.rgba[((size_t)(py + r) * p.w + px) * 4];
                const uint8_t* src = bmp + (size_t)r * gw;
                for (int c = 0; c < gw; ++c) {
                    dst[c * 4 + 0] = 255;
                    dst[c * 4 + 1] = 255;
                    dst[c * 4 + 2] = 255;
                    dst[c * 4 + 3] = src[c];
                }
            }
            // Upload only the glyph rect (rows are stored top-down; the
            // texture is flipped so updateRegion converts back). updateRegion
            // takes tightly packed rows and the page rows are strided by the
            // page width, so pack the glyph into a scratch buffer first - once
            // per glyph, not per frame.
            for (int r = 0; r < gh; ++r) {
                std::memcpy(&tight[(size_t)r * (size_t)gw * 4],
                            &p.rgba[((size_t)(py + r) * p.w + px) * 4],
                            (size_t)gw * 4);
            }
            p.tex.updateRegion(tight.data(), px, py, gw, gh, 4);
            p.cursorX += gw + 1;
            p.rowH = std::max(p.rowH, gh);
            g.uv = {(float)px, (float)py, (float)gw, (float)gh};
            g.page = pageIdx;
        }
    }
    if (bmp) stbtt_FreeBitmap(bmp, nullptr);

    auto res = glyphs_.emplace(cp, g);
    return &res.first->second;
}

float Font::advanceOf(uint32_t cp, float scale) const {
    const GlyphInfo* g = glyphFor(cp);
    return g ? g->advance * scale : 0.0f;
}

// ---------------------------------------------------------------- metrics
Vec2 Font::measure(const std::string& text, float scale) const {
    if (!valid()) return {0, 0};
    auto cps = decodeUtf8(text);
    float w = 0, maxW = 0;
    int lines = 1;
    for (uint32_t cp : cps) {
        if (cp == '\n') { maxW = std::max(maxW, w); w = 0; ++lines; continue; }
        w += advanceOf(cp, scale);
    }
    maxW = std::max(maxW, w);
    return {maxW, lineHeight() * scale * (float)lines};
}

float Font::widthOf(const std::string& text, float scale) const {
    return measure(text, scale).x;
}

std::vector<std::string> Font::wrapLines(const std::string& text, float maxWidth,
                                         float scale) const {
    std::vector<std::string> lines;
    if (maxWidth <= 0) { lines.push_back(text); return lines; }

    // Split on explicit newlines first.
    std::vector<std::string> paragraphs;
    {
        std::string cur;
        for (char ch : text) {
            if (ch == '\n') { paragraphs.push_back(cur); cur.clear(); }
            else cur.push_back(ch);
        }
        paragraphs.push_back(cur);
    }

    for (const std::string& para : paragraphs) {
        if (para.empty()) { lines.emplace_back(); continue; }
        std::vector<uint32_t> cps = decodeUtf8(para);
        std::string line;
        float lineW = 0;

        for (size_t i = 0; i < cps.size();) {
            // Gather the next word (including its leading spaces).
            size_t j = i;
            while (j < cps.size() && cps[j] == ' ') ++j;
            size_t wordEnd = j;
            while (wordEnd < cps.size() && cps[wordEnd] != ' ') ++wordEnd;

            float spacesW = 0;
            for (size_t k = i; k < j; ++k) spacesW += advanceOf(cps[k], scale);
            float wordW = 0;
            for (size_t k = j; k < wordEnd; ++k) wordW += advanceOf(cps[k], scale);

            if (lineW + spacesW + wordW <= maxWidth || line.empty()) {
                for (size_t k = i; k < j; ++k) encodeUtf8(cps[k], line);
                lineW += spacesW;
                // Break a single word that is wider than the line.
                if (wordW > maxWidth) {
                    for (size_t k = j; k < wordEnd; ++k) {
                        float aw = advanceOf(cps[k], scale);
                        if (lineW + aw > maxWidth && !line.empty()) {
                            lines.push_back(line);
                            line.clear();
                            lineW = 0;
                        }
                        encodeUtf8(cps[k], line);
                        lineW += aw;
                    }
                } else {
                    for (size_t k = j; k < wordEnd; ++k) encodeUtf8(cps[k], line);
                    lineW += wordW;
                }
            } else {
                lines.push_back(line);
                line.clear();
                lineW = 0;
                for (size_t k = j; k < wordEnd; ++k) encodeUtf8(cps[k], line);
                lineW = wordW;
            }
            i = wordEnd;
        }
        lines.push_back(line);
    }
    return lines;
}

float Font::heightWrapped(const std::string& text, float maxWidth, float scale) const {
    if (!valid()) return 0;
    auto lines = wrapLines(text, maxWidth, scale);
    return lineHeight() * scale * (float)std::max<size_t>(1, lines.size());
}

float Font::widthWrapped(const std::string& text, float maxWidth, float scale) const {
    if (!valid()) return 0;
    auto lines = wrapLines(text, maxWidth, scale);
    float w = 0;
    for (auto& l : lines) w = std::max(w, measure(l, scale).x);
    return w;
}

// ---------------------------------------------------------------- drawing
void Font::draw(SpriteBatch& batch, const std::string& text, Vec2 pos,
                float scale, Color c) const {
    if (!valid()) return;
    auto cps = decodeUtf8(text);
    float penX = pos.x;
    // pos is the TOP-LEFT of the text block (Y-up batch space).
    float baseline = pos.y - ascent_ * scale;
    float lh = lineHeight() * scale;

    if (bitmap_) {
        // BMFont offsets are measured from the top-left of the line, not from
        // the baseline, so draw directly from `pos` and step down per line.
        float lineTop = pos.y;
        for (uint32_t cp : cps) {
            if (cp == '\n') { penX = pos.x; lineTop -= lh; continue; }
            const GlyphInfo* g = glyphFor(cp);
            if (!g) continue;
            if (g->size.x > 0 && g->size.y > 0 && g->page < (int)pages_.size()) {
                const Page& p = pages_[(size_t)g->page];
                float atlW = (float)p.tex.width(), atlH = (float)p.tex.height();
                if (atlW <= 0 || atlH <= 0) { penX += g->advance * scale; continue; }
                SpriteDraw d;
                d.tex = &p.tex;
                d.pos = {penX + g->offset.x * scale + g->size.x * scale * 0.5f,
                         lineTop - g->offset.y * scale - g->size.y * scale * 0.5f};
                d.size = {g->size.x * scale, g->size.y * scale};
                d.color = c;
                d.src = {g->uv.x / atlW, 1.0f - (g->uv.y + g->uv.h) / atlH,
                         g->uv.w / atlW, g->uv.h / atlH};
                batch.draw(d);
            }
            penX += g->advance * scale;
        }
        return;
    }

    for (uint32_t cp : cps) {
        if (cp == '\n') { penX = pos.x; baseline -= lh; continue; }
        if (cp == ' ' || cp == '\t') { penX += advanceOf(cp, scale); continue; }
        const GlyphInfo* g = glyphFor(cp);
        if (!g) continue;
        if (g->size.x > 0 && g->size.y > 0 && g->page < (int)pages_.size()) {
            const Page& p = pages_[(size_t)g->page];
            float atlW = (float)p.w, atlH = (float)p.h;
            // stb xoff/yoff are Y-down offsets from the baseline pen:
            // bitmap top-left = (penX + xoff, baseline + yoff) in Y-down.
            // Convert to Y-up: top = baseline - yoff, center = top - h/2.
            Vec2 gp{
                penX + g->offset.x * scale + g->size.x * scale * 0.5f,
                baseline - g->offset.y * scale - g->size.y * scale * 0.5f
            };
            SpriteDraw d;
            d.tex = &p.tex;
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

void Font::drawWrapped(SpriteBatch& batch, const std::string& text, Vec2 pos,
                       float maxWidth, float scale, Color c) const {
    if (!valid()) return;
    auto lines = wrapLines(text, maxWidth, scale);
    float y = pos.y;
    for (auto& line : lines) {
        draw(batch, line, {pos.x, y}, scale, c);
        y -= lineHeight() * scale;
    }
}

void Font::drawWrappedCentered(SpriteBatch& batch, const std::string& text, Vec2 topCenter,
                               float maxWidth, float scale, Color c) const {
    if (!valid()) return;
    auto lines = wrapLines(text, maxWidth, scale);
    float y = topCenter.y;
    for (auto& line : lines) {
        float w = measure(line, scale).x;
        draw(batch, line, {topCenter.x - w * 0.5f, y}, scale, c);
        y -= lineHeight() * scale;
    }
}

} // namespace forge
