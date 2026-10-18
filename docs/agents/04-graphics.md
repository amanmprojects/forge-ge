# 04 — Graphics

Headers: `forge/graphics/sprite_batch.hpp`, `camera.hpp`, `texture.hpp`,
`font.hpp`, `framebuffer.hpp`, `tilemap.hpp`, `shader.hpp`, `gl_loader.hpp`.

## SpriteBatch

A deferred quad batcher: every draw call you make copies one quad's vertices
into a staging buffer; a *flush* uploads and draws everything accumulated so
far. One flush = one `glDrawElements` = one draw call, regardless of how many
sprites it contains (up to limits).

```cpp
class SpriteBatch {
    static constexpr int MAX_QUADS = 8192;   // per flush
    static constexpr int MAX_SLOTS = 16;     // distinct textures per flush

    bool init();  void shutdown();  bool valid() const;

    void begin(const Mat4& viewProj);   // start a pass (records FBO + viewport)
    void draw(const SpriteDraw& d);     // submit one quad
    void end();                         // flush and unbind

    void drawTexture(const Texture2D& tex, Vec2 pos, Vec2 size,
                     float rot = 0, Color c = Color::white());
    void drawTextureRegion(const Texture2D& tex, Vec2 pos, Vec2 size,
                           Rect pixelSrc, float rot = 0, Color c = Color::white());
    void drawRect(Rect r, Color c);                                  // filled
    void drawRectOutline(Rect r, Color c, float thickness = 1.0f);
    void drawCircle(Vec2 center, float radius, Color c, int segments = 24);
    void drawCircleOutline(Vec2 center, float radius, Color c,
                           float thickness = 2.0f, int segments = 48);
    void drawLine(Vec2 a, Vec2 b, Color c, float thickness = 1.0f);

    int  drawCalls() const;  int quadsDrawn() const;  void resetStats();
};
```

### The contract

- One pass: `begin(viewProj)` … `end()`. `end()` flushes; `begin()` on an
  un-ended batch warns and flushes into the *old* target, so don't rely on it.
- Everything you draw in a pass uses the same view-projection matrix. World
  pass with `cam.viewProj(w, h)`, HUD pass with `Engine::defaultProj()`
  (screen-centered pixels, Y-up).
- Draw order is submission order (painter's algorithm). There is **no depth
  buffer use**; later quads cover earlier ones. Use it deliberately for
  layering, or `Sprite::layer`/`Scene::draw` (sorted).
- Blending is `SRC_ALPHA, ONE_MINUS_SRC_ALPHA`, enabled once at init. There is
  no additive blend mode — premultiplied-style bright effects are done with
  color choice (see `08_snake`'s glows).

### Flush rules

A flush happens when:

1. `end()` is called, or
2. the batch is full (8192 quads), or
3. a 17th distinct texture appears in the same pass, or
4. `begin()` is called again for a new pass.

Minimize draw calls by drawing same-texture sprites consecutively and keeping
distinct textures per pass ≤ 16. `engine.drawCalls()` reflects flushes.

### SpriteDraw

```cpp
struct SpriteDraw {
    const Texture2D* tex = nullptr;  // nullptr = 1×1 white (shapes/text too)
    Vec2 pos{0,0};                   // CENTER of the quad
    Vec2 size{32,32};                // full width/height
    float rotation = 0.0f;           // radians, CCW
    Vec2 origin{0.5f,0.5f};          // normalized pivot inside the quad
    Rect src{0,0,1,1};               // UV rect in 0..1 — NOT pixels
    Color color = Color::white();
    bool flipX = false, flipY = false;
};
```

- `pos` is the center of the quad. With `origin = {0,0}` the sprite pivots
  around its bottom-left *local* corner — useful for health bars rotating
  around an end, or turret barrels. Pivot math is
  `local = (0.5 - origin) * size`.
- `src` is the **UV** rect. For atlas regions in pixels use
  `drawTextureRegion` (top-left pixel rect; it handles the flipY conversion),
  or convert manually: `{px.x/tw, 1 - (px.y+px.h)/th, px.w/tw, px.h/th}`.
- `color` multiplies the texture/white quad; pass `Color{..., alpha}` for
  transparency. There is no tint interpolation — animate it yourself.

### Shapes

Rect/line/circle helpers all draw through the same batch using the white
texture, so they mix freely with sprites and text without extra state changes.

- `drawCircle` clamps `segments` to 6..64 and emits a triangle fan built from
  degenerate quads (24 segments is the default and looks fine at game sizes).
- `drawCircleOutline` tessellates into line segments (min 8); thick outlines
  are quads, so huge thickness relative to radius can self-overlap.
- `drawRectOutline` draws four filled rects inward from the edges; `thickness`
  is in pixels and must be ≤ half the smaller side.
- `drawLine` builds a rotated quad of `thickness`; zero-length lines are
  skipped. Lines to not support rounded caps.

### Drawing into a framebuffer

```cpp
forge::Framebuffer fb;
fb.create(640, 480);          // RGBA8 + depth-less FBO, CLAMP + LINEAR

fb.bind();                    // binds FBO and sets viewport to 640×480
engine.clear(0.0f, 0.0f, 0.0f);
auto& batch = engine.batch();
batch.begin(cam.viewProj(fb.width(), fb.height()));
game.draw(batch);
batch.end();                  // flush happens while the FBO is bound
forge::Framebuffer::unbind(engine.viewport().x, engine.viewport().y); // back to screen
```

`begin()` captures the current framebuffer + viewport, so even a mistaken
un-ended batch flushes into the right target. The FBO's color texture is
available as `fb.texture()` (`uint32_t` GL id) for drawing into another pass —
it samples upright through the batch with default `src` (render targets and
flipY-loaded images share the same bottom-up GL orientation). Readback is the
only place orientation bites: `glReadPixels` returns rows bottom-up, so a PNG
writer must flip them (see 09-recipes "Deterministic headless capture").

## Camera2D

```cpp
class Camera2D {
    Vec2 position{0,0};  float zoom = 1.0f;  float rotation = 0.0f; // CCW rad

    Mat4 viewProj(int vpW, int vpH) const;
    Vec2 screenToWorld(Vec2 screenPx, int vpW, int vpH) const;
    Vec2 worldToScreen(Vec2 world, int vpW, int vpH) const;

    void addTrauma(float amount);           // 0..1, accumulates (clamped)
    void update(float dt);                  // decays trauma; advances shake noise
    Vec2 shakeOffset() const;
    float trauma() const;
    void setTraumaDecay(float d);           // default 1.6 /s
    void setMaxShake(float px);             // default 24 px

    void follow(Vec2 target, float smoothing, float dt); // exp approach
};
```

- The view transform translates the world into camera space first, then
  rotates and zooms about the camera center — so zooming never drifts and
  picking stays correct (they share the math with `screenToWorld`).
- `screenToWorld` expects SDL-style input coords (**top-left origin**, drawable
  pixels) and returns world units; `worldToScreen` is its inverse (top-left
  output). Both account for rotation, zoom and the current shake offset.
- Shake: `addTrauma(0..1)` accumulates, `update(dt)` decays linearly at
  `traumaDecay`, and `shakeOffset()` returns `trauma² * maxShake` modulated by
  deterministic `valueNoise2D` (smooth, reproducible — not per-frame jitter).
  You must call `update(dt)` every frame or trauma never decays.
- `follow` implements exponential smoothing
  (`position = lerp(position, target, 1 - exp(-smoothing*dt))`); pick
  `smoothing` ≈ 4–10.

```cpp
cam.follow(player.pos, 8.0f, dt);
cam.update(dt);                       // decay + shake time
forge::Mat4 vp = cam.viewProj(vpSize.x, vpSize.y);
```

## Texture2D

```cpp
enum class TextureFilter { Nearest, Linear };
enum class TextureWrap   { Clamp, Repeat };

bool loadFile(const std::string& path, bool flipY = true,
              TextureFilter filter = TextureFilter::Linear,
              TextureWrap wrap = TextureWrap::Clamp);
bool loadMemory(const uint8_t* data, int w, int h, int channels,   // raw pixels
                TextureFilter filter = TextureFilter::Linear,
                TextureWrap wrap = TextureWrap::Clamp);
bool createSolid(Color c, TextureFilter filter = TextureFilter::Nearest);
bool createChecker(int cells, Color a, Color b, int cellPx = 8);

bool valid() const;  void bind(int slot = 0) const;
int width() const;   int height() const;  Vec2 size() const;  uint32_t id() const;
const std::string& path() const;
static Texture2D& white();   // shared 1×1 white (for shapes; after GL init)
```

- Formats: PNG/JPG/BMP/TGA (stb_image), always decoded to RGBA.
- **`flipY = true` is the engine convention**: UV `(0,0)` = image bottom-left,
  so pixel-rect helpers (`drawTextureRegion`, `Sprite::src`, `Tilemap`) can
  take ordinary top-left image coordinates. Only pass `flipY = false` if you
  *want* the image upside down.
- `loadMemory` uploads rows as given: row 0 of your buffer becomes the
  **bottom** of the texture (GL convention). When mixing raw uploads with the
  pixel-rect helpers, supply rows bottom-up — `02_platformer` builds its
  procedural tileset flipped for exactly this reason.
- `createSolid`/`createChecker` are the zero-asset prototypes; `createChecker`
  is Repeat-wrapped by design (tile it with a quad of any size).
- `white()` is a lazily created shared 1×1 texture — don't destroy it; floor
  shapes inherit its "texture" so they ride the same batch.

## Font

TTF via stb_truetype, baked once to a 512×512 alpha atlas containing printable
ASCII (32–126). Everything else renders as `'?'`. No Unicode, no SDF.

```cpp
bool loadFile(const std::string& path, float pixelHeight = 32.0f);
bool loadMemory(const uint8_t* ttfData, size_t len, float pixelHeight = 32.0f);
void destroy();  bool valid() const;

float height() const;       // baked pixel height
float lineHeight() const;   // pixelHeight * 1.25
Vec2  measure(const std::string& text, float scale = 1.0f) const; // \\n aware
float widthOf(const std::string& text, float scale = 1.0f) const;

void draw(SpriteBatch&, const std::string& text, Vec2 pos, float scale, Color) const;
void drawCentered(SpriteBatch&, const std::string& text, Vec2 center, float scale, Color) const;
```

- `draw` treats `pos` as the **top-left of the first line** (Y-up space), and
  falls downward for `\n`. `drawCentered` centers the whole block on `center`.
- `scale` is a multiplier on the baked size; bake at a size near the largest
  you'll display for crisp results (avoid upscaling much).
- The bake fails (returns false) if the atlas overflows — practically this
  means keep `pixelHeight` modest (≈ ≤ 40 px for typical fonts).
- Example convention for a system font lookup (there is no bundled font):

```cpp
const char* candidates[] = {
    "/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf",
    "/usr/share/fonts/Adwaita/AdwaitaMono-Regular.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
};
```

## Tilemap

CSV or Tiled-JSON tile layers, with culling, per-id solid flags and rect
collision. Y-up, tile `(0,0)` is the **bottom-left**.

```cpp
void create(int w, int h, int tileW, int tileH);      // one empty "main" layer
int  addLayer(const std::string& name);               // returns layer index
bool loadCsv(const std::string& csv, int w, int h, int tileW, int tileH);
bool loadTiledJson(const std::string& path);          // setTileset() afterwards

void setTileset(const Texture2D* tex, int columns, int tileCount);
void setTile(int layer, int x, int y, int id);        // id is 1-based; 0 clears
int  tile(int layer, int x, int y) const;

void setSolid(int tileId, bool solid = true);         // per-id flag, any layer
bool isSolid(int tileId) const;
std::vector<Rect> solidsInRect(Rect r) const;         // world AABBs
bool collideRect(Rect r, Vec2* resolve = nullptr) const; // hit + total push-out

void draw(SpriteBatch&, Vec2 origin, Rect view) const;  // culled to view
void drawAll(SpriteBatch&, Vec2 origin) const;
Vec2 worldSize() const;
```

Format notes:

- **CSV**: a flat comma list, row-major with **row 0 = bottom** (Y-up). Values
  are 1-based ids, `0` empty. `loadCsv` reads at most `w*h` values and returns
  false if fewer were present.
- **Tiled JSON**: only the **first tileset** is used; its image path is
  exposed via `tilesetPath()` for you to load; `firstgid` is remapped to local
  1-based ids; Tiled's top-down rows are flipped to Y-up on load; tile
  flip/diagonal flags are **masked (ignored)**; a tileset tile size that
  differs from the map tile size is rejected with a log error. You must call
  `setTileset(&texture, columns, tileCount)` with the loaded image before
  drawing (columns = tiles per row in the sheet).
- `setSolid(id)` marks a tile id solid for `solidsInRect`/`collideRect` on
  **all layers**; layer visibility does not affect solids (hiding a collision
  layer does not open walls).
- `collideRect` returns true when `r` overlaps any solid, and writes the
  **sum of per-tile push-outs** into `resolve` (each tile contributes its
  smallest-axis MTV). Apply it yourself:

```cpp
forge::Vec2 fix;
if (map.collideRect(playerBox, &fix)) {
    playerBox.x += fix.x;  player.pos.x += fix.x;
    playerBox.y += fix.y;  player.pos.y += fix.y;
}
```

Because the pushes are accumulated once, run the query again after applying
for an exact resolution (2 iterations is plenty for single-tile-thick walls).

- `draw` culls to a world-space `view` rect (usually the camera's visible
  rect), keyed off `origin` = world position of tile (0,0).

## Shader and the GL loader

Custom shaders are possible but rarely needed — the batch owns the only
shader the engine uses.

```cpp
forge::Shader sh;                                    // pos/uv/color/texindex sprite shader
sh.load(vertSrc, fragSrc);  sh.loadFiles(v, f);
sh.bind();  sh.setInt/setFloat/setVec2/setVec4/setMat4(name, …);
sh.setTextureSlots("uTextures", slots, count);
```

`forge::gl::*` exposes ~60 GL 3.3 functions (`glEnable`, `glBlendFunc`,
`glScissor`, `glReadPixels`, framebuffer/texture calls, …) loaded through
SDL. Include `<forge/forge.hpp>` and use `forge::gl::glClear(...)` etc.; do
not include a system GL header (the loader defines its own typedefs and
constants, headers intentionally have no GL dependency).

```cpp
forge::gl::glEnable(forge::gl::GL_SCISSOR_TEST);   // e.g. clip a panel
forge::gl::glScissor(x, y, w, h);
// … draw …
forge::gl::glDisable(forge::gl::GL_SCISSOR_TEST);
```

Remember scissor rects are bottom-left origin, and the batch does not reset
scissor state for you.

## Performance notes

- 10,000 sprites batch into **2 draw calls** in `03_benchmark` (one flush at
  8192 quads + one for the rest, single texture).
- Batching cost is per `draw()`, not per flush: each call writes 4 vertices
  (80 bytes) into the staging buffer.
- The VBO is orphaned each flush (`glBufferData(..., nullptr)`) before the
  subdata upload to avoid pipeline stalls — don't try to "optimize" that away.
- Sorting sprites by texture within a pass is the single biggest win for
  multi-texture scenes; keep ≤ 16 textures per pass to avoid mid-pass flushes.
