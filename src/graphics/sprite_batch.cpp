#include "forge/graphics/sprite_batch.hpp"
#include "forge/graphics/gl_loader.hpp"
#include "forge/core/logger.hpp"
#include <cstring>
#include <cmath>

namespace forge {

SpriteBatch::SpriteBatch() {
    mapped_ = new Vertex[MAX_VERTS];
}

SpriteBatch::~SpriteBatch() {
    shutdown();
    delete[] mapped_;
}

bool SpriteBatch::init() {
    if (!gl::isLoaded()) {
        FORGE_LOG_ERROR("SpriteBatch::init: GL not loaded");
        return false;
    }
    if (!shader_.load(Shader::kSpriteVert, Shader::kSpriteFrag)) return false;

    gl::glGenVertexArrays(1, &vao_);
    gl::glBindVertexArray(vao_);

    gl::glGenBuffers(1, &vbo_);
    gl::glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    gl::glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * MAX_VERTS, nullptr, GL_DYNAMIC_DRAW);

    // Precomputed index buffer: 0,1,2, 2,3,0 per quad
    static uint32_t indices[MAX_INDICES];
    static bool built = false;
    if (!built) {
        for (int q = 0; q < MAX_QUADS; ++q) {
            uint32_t v = q * 4;
            uint32_t* p = &indices[q * 6];
            p[0] = v + 0; p[1] = v + 1; p[2] = v + 2;
            p[3] = v + 2; p[4] = v + 3; p[5] = v + 0;
        }
        built = true;
    }
    gl::glGenBuffers(1, &ibo_);
    gl::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
    gl::glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    // layout: pos(2f) uv(2f) color(4ub) texIndex(1f)
    gl::glEnableVertexAttribArray(0);
    gl::glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (const void*)offsetof(Vertex, x));
    gl::glEnableVertexAttribArray(1);
    gl::glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (const void*)offsetof(Vertex, u));
    gl::glEnableVertexAttribArray(2);
    gl::glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (const void*)offsetof(Vertex, color));
    gl::glEnableVertexAttribArray(3);
    gl::glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(Vertex), (const void*)offsetof(Vertex, texIndex));

    gl::glBindVertexArray(0);
    gl::glBindBuffer(GL_ARRAY_BUFFER, 0);

    gl::glEnable(GL_BLEND);
    gl::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl::glDisable(GL_DEPTH_TEST);

    FORGE_LOG_INFO("SpriteBatch ready (max %d quads/batch)", MAX_QUADS);
    return true;
}

void SpriteBatch::shutdown() {
    if (!gl::isLoaded()) { vao_ = vbo_ = ibo_ = 0; return; }
    if (ibo_) { gl::glDeleteBuffers(1, &ibo_); ibo_ = 0; }
    if (vbo_) { gl::glDeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (vao_) { gl::glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    shader_.destroy();
}

void SpriteBatch::begin(const Mat4& viewProj) {
    quadCount_ = 0;
    slotCount_ = 0;
    std::memset(slots_, 0, sizeof(slots_));
    shader_.bind();
    shader_.setMat4("uProj", viewProj);
    gl::glBindVertexArray(vao_);
    gl::glBindBuffer(GL_ARRAY_BUFFER, vbo_);
}

float SpriteBatch::slotFor(const Texture2D* tex) {
    if (!tex || !tex->valid()) tex = &Texture2D::white();
    for (int i = 0; i < slotCount_; ++i)
        if (slots_[i] == tex) return (float)i;
    if (slotCount_ >= MAX_SLOTS || quadCount_ >= MAX_QUADS) flush();
    // re-check after flush
    for (int i = 0; i < slotCount_; ++i)
        if (slots_[i] == tex) return (float)i;
    slots_[slotCount_] = tex;
    return (float)(slotCount_++);
}

void SpriteBatch::draw(const SpriteDraw& d) {
    const Texture2D* tex = (d.tex && d.tex->valid()) ? d.tex : &Texture2D::white();
    // Find existing slot
    int slot = -1;
    for (int i = 0; i < slotCount_; ++i) {
        if (slots_[i] == tex) { slot = i; break; }
    }
    if (slot < 0) {
        // Need a new slot: flush if batch or slots are full
        if (quadCount_ >= MAX_QUADS || slotCount_ >= MAX_SLOTS) flush();
        slots_[slotCount_] = tex;
        slot = slotCount_++;
    } else if (quadCount_ >= MAX_QUADS) {
        flush();
        // Re-insert after flush (slot indices were cleared)
        slots_[slotCount_] = tex;
        slot = slotCount_++;
    }

    // quad corners in local space around pivot
    float hw = d.size.x * 0.5f, hh = d.size.y * 0.5f;
    // origin (0.5,0.5) = center; offset from center:
    float ox = (0.5f - d.origin.x) * d.size.x;
    float oy = (0.5f - d.origin.y) * d.size.y;
    Vec2 local[4] = {
        {-hw + ox, -hh + oy}, {hw + ox, -hh + oy},
        {hw + ox, hh + oy}, {-hw + ox, hh + oy},
    };
    float c = std::cos(d.rotation), s = std::sin(d.rotation);
    float u0 = d.src.x, v0 = d.src.y, u1 = d.src.x + d.src.w, v1 = d.src.y + d.src.h;
    if (d.flipX) std::swap(u0, u1);
    if (d.flipY) std::swap(v0, v1);
    float uv[4][2] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
    uint32_t col = d.color.pack();

    Vertex* v = &mapped_[quadCount_ * 4];
    for (int i = 0; i < 4; ++i) {
        float rx = local[i].x * c - local[i].y * s;
        float ry = local[i].x * s + local[i].y * c;
        v[i].x = d.pos.x + rx;
        v[i].y = d.pos.y + ry;
        v[i].u = uv[i][0];
        v[i].v = uv[i][1];
        v[i].color = col;
        v[i].texIndex = (float)slot;
    }
    ++quadCount_;
}

void SpriteBatch::flush() {
    if (quadCount_ == 0) return;
    for (int i = 0; i < slotCount_; ++i) {
        if (slots_[i]) slots_[i]->bind(i);
    }
    gl::glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(Vertex) * quadCount_ * 4, mapped_);
    gl::glDrawElements(GL_TRIANGLES, quadCount_ * 6, GL_UNSIGNED_INT, nullptr);
    ++drawCalls_;
    quadsDrawn_ += quadCount_;
    quadCount_ = 0;
    slotCount_ = 0;
    std::memset(slots_, 0, sizeof(slots_));
}

void SpriteBatch::end() {
    flush();
    gl::glBindVertexArray(0);
}

void SpriteBatch::drawTexture(const Texture2D& tex, Vec2 pos, Vec2 size, float rot, Color c) {
    SpriteDraw d;
    d.tex = &tex; d.pos = pos; d.size = size; d.rotation = rot; d.color = c;
    draw(d);
}

void SpriteBatch::drawTextureRegion(const Texture2D& tex, Vec2 pos, Vec2 size, Rect pixelSrc,
                                    float rot, Color c) {
    SpriteDraw d;
    d.tex = &tex; d.pos = pos; d.size = size; d.rotation = rot; d.color = c;
    float tw = (float)tex.width(), th = (float)tex.height();
    if (tw > 0 && th > 0) {
        // pixelSrc is top-left origin; textures are flipped on load (GL bottom-left)
        d.src = {pixelSrc.x / tw, 1.0f - (pixelSrc.y + pixelSrc.h) / th,
                 pixelSrc.w / tw, pixelSrc.h / th};
    }
    draw(d);
}

void SpriteBatch::drawRect(Rect r, Color c) {
    SpriteDraw d;
    d.tex = nullptr;
    d.pos = r.center(); d.size = r.size(); d.color = c;
    draw(d);
}

void SpriteBatch::drawRectOutline(Rect r, Color c, float thickness) {
    float t = thickness;
    drawRect({r.x, r.y, r.w, t}, c);
    drawRect({r.x, r.y + r.h - t, r.w, t}, c);
    drawRect({r.x, r.y + t, t, r.h - 2 * t}, c);
    drawRect({r.x + r.w - t, r.y + t, t, r.h - 2 * t}, c);
}

void SpriteBatch::drawCircle(Vec2 center, float radius, Color c, int segments) {
    // triangle fan around center using quads-as-tris: emit (segments) quads
    // degenerated: simpler — approximate with N quads of pie slices via 2 tris each.
    // For batch simplicity, draw as filled polygon fan with center vertex duplicated.
    if (segments < 6) segments = 6;
    if (segments > 64) segments = 64;
    for (int i = 0; i < segments; ++i) {
        float a0 = TAU * i / segments, a1 = TAU * (i + 1) / segments;
        Vec2 p0{center.x + std::cos(a0) * radius, center.y + std::sin(a0) * radius};
        Vec2 p1{center.x + std::cos(a1) * radius, center.y + std::sin(a1) * radius};
        // emit as thin quad (center, p0, p1, p1) — last vert degenerate
        const Texture2D* tex = &Texture2D::white();
        int slot = -1;
        for (int k = 0; k < slotCount_; ++k)
            if (slots_[k] == tex) { slot = k; break; }
        if (slot < 0) {
            if (quadCount_ >= MAX_QUADS || slotCount_ >= MAX_SLOTS) flush();
            slots_[slotCount_] = tex;
            slot = slotCount_++;
        } else if (quadCount_ >= MAX_QUADS) {
            flush();
            slots_[slotCount_] = tex;
            slot = slotCount_++;
        }
        uint32_t col = c.pack();
        Vertex* v = &mapped_[quadCount_ * 4];
        v[0] = {center.x, center.y, 0.5f, 0.5f, col, (float)slot};
        v[1] = {p0.x, p0.y, 0.5f, 0.5f, col, (float)slot};
        v[2] = {p1.x, p1.y, 0.5f, 0.5f, col, (float)slot};
        v[3] = {p1.x, p1.y, 0.5f, 0.5f, col, (float)slot};
        ++quadCount_;
    }
}

void SpriteBatch::drawCircleOutline(Vec2 center, float radius, Color c, float thickness, int segments) {
    if (segments < 8) segments = 8;
    for (int i = 0; i < segments; ++i) {
        float a0 = TAU * i / segments, a1 = TAU * (i + 1) / segments;
        Vec2 p0{center.x + std::cos(a0) * radius, center.y + std::sin(a0) * radius};
        Vec2 p1{center.x + std::cos(a1) * radius, center.y + std::sin(a1) * radius};
        drawLine(p0, p1, c, thickness);
    }
}

void SpriteBatch::drawLine(Vec2 a, Vec2 b, Color c, float thickness) {
    Vec2 d = b - a;
    float len = d.length();
    if (len < EPSILON) return;
    Vec2 dir = d / len;
    Vec2 n{-dir.y, dir.x};
    float h = thickness * 0.5f;
    const Texture2D* tex = &Texture2D::white();
    int slot = -1;
    for (int k = 0; k < slotCount_; ++k)
        if (slots_[k] == tex) { slot = k; break; }
    if (slot < 0) {
        if (quadCount_ >= MAX_QUADS || slotCount_ >= MAX_SLOTS) flush();
        slots_[slotCount_] = tex;
        slot = slotCount_++;
    } else if (quadCount_ >= MAX_QUADS) {
        flush();
        slots_[slotCount_] = tex;
        slot = slotCount_++;
    }
    uint32_t col = c.pack();
    Vertex* v = &mapped_[quadCount_ * 4];
    v[0] = {a.x - n.x * h, a.y - n.y * h, 0.5f, 0.5f, col, (float)slot};
    v[1] = {b.x - n.x * h, b.y - n.y * h, 0.5f, 0.5f, col, (float)slot};
    v[2] = {b.x + n.x * h, b.y + n.y * h, 0.5f, 0.5f, col, (float)slot};
    v[3] = {a.x + n.x * h, a.y + n.y * h, 0.5f, 0.5f, col, (float)slot};
    ++quadCount_;
}

} // namespace forge
