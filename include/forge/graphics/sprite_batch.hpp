#pragma once
// SpriteBatch: single-draw-call-per-flush quad batcher.
//   - MAX_QUADS quads per batch (default 8192 = 32k verts), dynamic VBO
//   - 16 texture slots per batch (GL limit for uniform sampler array)
//   - begin(cam, viewport) -> draw(...) -> end() flushes
// Also draws rects/lines/circles via the white texture (ShapeRenderer below
// reuses the same batch for zero extra state changes).

#include "forge/math/math.hpp"
#include "forge/graphics/shader.hpp"
#include "forge/graphics/texture.hpp"
#include "forge/graphics/camera.hpp"
#include <cstdint>

namespace forge {

struct SpriteDraw {
    const Texture2D* tex = nullptr; // null = white
    Vec2 pos{0, 0};                 // center
    Vec2 size{32, 32};
    float rotation = 0.0f;          // radians CCW
    Vec2 origin{0.5f, 0.5f};        // normalized pivot
    Rect src{0, 0, 1, 1};           // UV rect in 0..1 (use pixelRect for atlas)
    Color color = Color::white();
    bool flipX = false, flipY = false;
};

class SpriteBatch {
public:
    static constexpr int MAX_QUADS = 8192;
    static constexpr int MAX_VERTS = MAX_QUADS * 4;
    static constexpr int MAX_INDICES = MAX_QUADS * 6;
    static constexpr int MAX_SLOTS = 16;

    SpriteBatch();
    ~SpriteBatch();

    SpriteBatch(const SpriteBatch&) = delete;
    SpriteBatch& operator=(const SpriteBatch&) = delete;

    bool init();      // call after GL context ready
    void shutdown();
    bool valid() const { return vao_ != 0; }

    void begin(const Mat4& viewProj);
    void draw(const SpriteDraw& d);
    void end();       // flush

    // Convenience overloads (all route into draw(SpriteDraw))
    void drawTexture(const Texture2D& tex, Vec2 pos, Vec2 size,
                     float rot = 0, Color c = Color::white());
    void drawTextureRegion(const Texture2D& tex, Vec2 pos, Vec2 size, Rect pixelSrc,
                           float rot = 0, Color c = Color::white());
    void drawRect(Rect r, Color c);                       // filled, white tex
    void drawRectOutline(Rect r, Color c, float thickness = 1.0f);
    void drawCircle(Vec2 center, float radius, Color c, int segments = 24);
    void drawCircleOutline(Vec2 center, float radius, Color c, float thickness = 2.0f, int segments = 48);
    void drawLine(Vec2 a, Vec2 b, Color c, float thickness = 1.0f);

    // Stats for profiling HUD
    int drawCalls() const { return drawCalls_; }
    int quadsDrawn() const { return quadsDrawn_; }
    void resetStats() { drawCalls_ = 0; quadsDrawn_ = 0; }

private:
    struct Vertex {
        float x, y;       // pos
        float u, v;       // uv
        uint32_t color;   // packed RGBA
        float texIndex;
    };

    void flush();
    float slotFor(const Texture2D* tex); // binds, returns slot index (flushes if full)

    uint32_t vao_ = 0, vbo_ = 0, ibo_ = 0;
    Shader shader_;
    Vertex* mapped_ = nullptr; // CPU-side staging buffer
    int quadCount_ = 0;
    const Texture2D* slots_[MAX_SLOTS] = {};
    int slotCount_ = 0;
    // Draw framebuffer + viewport recorded at begin(); an un-ended batch
    // flushes into this target, not whatever happens to be bound later.
    uint32_t boundFbo_ = 0;
    int boundVp_[4] = {0, 0, 0, 0};

    int drawCalls_ = 0;
    int quadsDrawn_ = 0;
};

} // namespace forge
