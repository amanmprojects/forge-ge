# 04 — Graphics

Headers: `forge/graphics/sprite_batch.hpp`, `camera.hpp`, `texture.hpp`,
`font.hpp`, `framebuffer.hpp`, `tilemap.hpp`, `shader.hpp`, `gl_loader.hpp`,
`atlas.hpp`, `lighting.hpp`, `postprocess.hpp`, `skeleton.hpp`.

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

    // UI helpers
    void drawNineSlice(const Texture2D& tex, Rect dst, Rect srcPixel, float border,
                       Color c = Color::white());
    void drawPanel(Rect r, Color fill, Color border, float thickness = 2.0f);
    void drawTextShadow(const Font& font, const std::string& text, Vec2 pos,
                        float scale, Color c, Color shadow = Color{0,0,0,0.7f});
    void drawQuad(Vec2 a, Vec2 b, Vec2 c, Vec2 d, Color col = Color::white(),
                  const Texture2D* tex = nullptr, Rect uv = {0, 0, 1, 1});

    // Blend modes (flush on change; reset to Alpha by begin())
    enum class BlendMode { Alpha, Additive, Multiply, None };
    void setBlendMode(BlendMode m);
    BlendMode blendMode() const;

    // Scissor clipping: framebuffer pixels, bottom-left origin
    void setScissor(Rect px);
    void clearScissor();

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
- Blending defaults to `SRC_ALPHA, ONE_MINUS_SRC_ALPHA` and is re-applied by
  `begin()`. Use `setBlendMode` for additive glows (`Additive`), shadow
  darkening (`Multiply`) or stencil-like masking (`None`). A mode change
  flushes, so group draws by mode. `end()` restores alpha blending so raw GL
  code after the pass sees sane state.

### Scissor clipping

```cpp
batch.setScissor({x, y, w, h});   // framebuffer pixels, BOTTOM-LEFT origin
batch.drawPanel(panel, fill, border);
batch.clearScissor();
```

The rect is in framebuffer pixels (not world, not screen-centered), which is
what `glScissor` wants — so a HUD panel at screen y=20 from the top needs
`fbH - y - h`. `setScissor` flushes first (already-submitted quads keep their
own clipping), `begin()` resets to off, and `end()` disables `GL_SCISSOR_TEST`
so nothing leaks into a following FBO pass or raw GL. Negative widths are
normalized. A scissor is clipped against the current viewport by GL itself, so
it works identically for the window and for an FBO.

### Blend modes and UI primitives

```cpp
batch.setBlendMode(SpriteBatch::BlendMode::Additive);
batch.drawCircle(glowPos, 40, Color{1, 0.8f, 0.3f, 0.5f});
batch.setBlendMode(SpriteBatch::BlendMode::Alpha);

batch.drawPanel({10, 10, 300, 120}, Color{0.05f, 0.06f, 0.09f, 0.9f},
                Color{1, 1, 1, 0.15f}, 2.0f);
batch.drawNineSlice(uiTex, {x, y, 240, 80}, {0, 0, 48, 48}, 16.0f);
batch.drawTextShadow(font, "PAUSED", center, 2.0f, Color::white());
```

`drawNineSlice` takes a **pixel** source rect and a border width in pixels; it
emits nine quads so corners never stretch. `drawPanel` is the zero-asset
equivalent (fill + inset outline). `drawQuad` is the raw escape hatch: four
arbitrary corners with one UV rect — useful for ramps, trails, gradient
strips and custom deformations (the quad is two triangles, so non-planar
corners in 2D are fine).

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
fb.create(640, 480, 4);       // 4x MSAA (renderbuffer + resolve target)

fb.bind();                    // binds FBO and sets viewport to 640×480
engine.clear(0.0f, 0.0f, 0.0f);
auto& batch = engine.batch();
batch.begin(cam.viewProj(fb.width(), fb.height()));
game.draw(batch);
batch.end();                  // flush happens while the FBO is bound
forge::Framebuffer::unbind(engine.viewport().x, engine.viewport().y); // back to screen
```

```cpp
class Framebuffer {
    bool create(int w, int h, int samples = 0);   // 0, 2, 4, 8
    bool resize(int w, int h);                    // keeps the sample count
    void destroy();  bool valid() const;
    void bind() const;                            // sets viewport to w×h
    static void unbind(int vpW, int vpH);         // back to the screen
    void resolve() const;                         // MSAA -> texture (no-op otherwise)
    bool readPixels(std::vector<uint8_t>& out) const; // RGBA8, bottom-up rows
    uint32_t texture() const;  int width() const;  int height() const;
    int samples() const;  bool multisampled() const;
};
```

- With `samples > 0`, `bind()` targets the multisampled FBO; the sampleable
  texture only receives the content on `resolve()` (which `readPixels()` calls
  for you). A driver that rejects the sample count silently falls back to a
  non-MSAA target, so always check `multisampled()` in tests.
- `resize()` recreates both attachments and keeps the sample count; existing
  `Texture2D` views of `texture()` must be re-adopted (`adoptRaw`).
- `Texture2D::adoptRaw(id, w, h, owns=false)` wraps a GL id you don't own
  (an FBO color texture) so you can pass it to `SpriteBatch`/`PostProcess`.
  Never let an owning `Texture2D` and an FBO share the id — use `owns=false`.

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

CSV or Tiled-JSON tile layers, with culling, multi-tileset support, flip
flags, infinite maps, object layers, per-layer parallax, autotiling, slopes,
per-id solid flags and rect collision. Y-up, tile `(0,0)` is the
**bottom-left**.

```cpp
void create(int w, int h, int tileW, int tileH);      // one empty "main" layer
int  addLayer(const std::string& name);               // returns layer index
bool loadCsv(const std::string& csv, int w, int h, int tileW, int tileH);
bool loadTiledJson(const std::string& path);          // tilesets not loaded yet
int  loadTilesets();                                  // load every referenced image

void setTileset(const Texture2D* tex, int columns, int tileCount); // legacy/1 set
void setTilesetTexture(int i, const Texture2D* tex);  // after loadTiledJson
const std::vector<TilesetRef>& tilesets() const;
int  tilesetFor(int localId) const;

void setTile(int layer, int x, int y, int id);        // id is 1-based; 0 clears
void setTileFlags(int layer, int x, int y, uint8_t flags); // 1=H 2=V 4=diagonal
int  tile(int layer, int x, int y) const;             // within-tileset id
int  tilesetIndex(int layer, int x, int y) const;
uint8_t tileFlags(int layer, int x, int y) const;

// per-layer presentation
TileLayer& layer(int i);  int layerCount() const;
//   name, visible, opacity, offset, parallax

void setSolid(int tileId, bool solid = true);         // per-id flag, any layer
bool isSolid(int tileId) const;
void setSlope(int tileId, float leftHeight, float rightHeight); // 0..1 of tileH
void clearSlope(int tileId);
TileSlope slope(int tileId) const;  bool isSlope(int tileId) const;
float slopeSurfaceHeight(int tileId, float t) const;
float surfaceYAt(int layer, int x, int y, float worldX) const;
std::vector<Rect> solidsInRect(Rect r) const;         // world AABBs
bool collideRect(Rect r, Vec2* resolve = nullptr) const; // hit + total push-out

// autotiling
void setAutoRule(const std::vector<int>& byMask);     // 256 entries, 0 = empty
void applyAutotile(int layer);
static std::vector<int> buildAutoRule(int baseId, int cornersId = 0,
                                      int edgesId = 0, int innerId = 0);

void draw(SpriteBatch&, Vec2 origin, Rect view) const;  // culled to view
void drawAll(SpriteBatch&, Vec2 origin) const;
Vec2 worldSize() const;
Vec2 originOffset() const;                             // infinite maps only
const std::vector<TilemapObject>& objects() const;
```

Format notes:

- **CSV**: a flat comma list, row-major with **row 0 = bottom** (Y-up). Values
  are 1-based ids, `0` empty. `loadCsv` reads at most `w*h` values and returns
  false if fewer were present.
- **Tiled JSON**: every tileset is kept (`firstgid` per set, resolved lazily).
  Tiled's top-down rows are flipped to Y-up on load. Flip/diagonal flags are
  preserved per tile (`tileFlags`, decoded by the renderer and exposed for
  gameplay if needed). A tileset tile size that differs from the map tile
  size is rejected with a log error.
- **Multi-tileset encoding**: a tile's id packs the tileset index in the high
  16 bits, so `tile()` returns the within-tileset id and `tilesetIndex()`
  tells you which sheet. Solid/slope rules therefore apply uniformly across
  sheets (`setSolid(3)` marks tile 3 in *every* tileset).
- **Infinite maps**: chunked `data` is expanded into dense bounds and
  `originOffset()` reports where world `(0,0)` sits relative to tile `(0,0)`
  (may be negative). Rendering, `solidsInRect` and `collideRect` all include
  the offset.
- **Object layers**: rectangles and points become `TilemapObject{name, type,
  point, rect, properties}` with Tiled's top-down Y converted to Y-up. Use
  them for spawn points, triggers and lights instead of parsing JSON twice.
- **Layers**: each has `visible`, `opacity`, `offset` and `parallax` (a
  multiplier applied around the camera in `draw`). Hiding a layer is purely
  visual — solids stay solid.
- `setSolid(id)` marks a tile id solid for `solidsInRect`/`collideRect` on
  **all layers**; layer visibility does not affect solids (hiding a collision
  layer does not open walls).
- **Autotiling**: `buildAutoRule` gives a simple 3-level rule (isolated →
  base, edges → edges, interior → inner); write your own 256-entry table for
  a real tileset. `applyAutotile(layer)` rewrites that layer from its
  non-empty neighbour mask. Do **not** apply it to a floor layer that is
  already fully populated — `rule[0]` is the "no neighbours" result.
- **Slopes**: `setSlope(id, left, right)` marks a tile solid and gives it a
  linear surface, where the heights are fractions of the tile height (0 =
  bottom, 1 = top). `(0, 1)` is a 45° ramp rising to the right; `(0.25, 0.75)`
  is a gentler one. `collideRect` resolves slope tiles **vertically only**,
  against the surface under the rect's horizontal center, and only pushes up
  (so walking up from below never traps you). Everything else still treats a
  slope tile as a full box for broadphase. `surfaceYAt`/`slopeSurfaceHeight`
  let platformer code place feet exactly on the ramp.
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

## AtlasPacker

A CPU-side shelf/skyline packer. No image decoding, no GL: you feed sizes (or
pixel rects), pack, blit your own pixels, then upload once.

```cpp
forge::AtlasPacker packer(1024, 1024, /*padding=*/2);
int a = packer.add(64, 32, "hero");
int b = packer.add(32, 32, "coin");
if (packer.pack()) {
    forge::Rect r = packer.rect(a);            // top-left pixel rect
    packer.blit(a, rgba.data(), 64, 32);       // copies into pixels()
    forge::Texture2D atlas;
    atlas.loadMemory(packer.pixels().data(), packer.width(), packer.height(), 4);
}
```

- `add` returns `-1` immediately when the entry can never fit the page; a
  layout where placed entries overflow makes `pack()` return false (entries
  never overlap — that is the invariant tests rely on).
- `padding` is a transparent gutter between entries, so linear filtering at
  tile edges cannot bleed neighbours.
- `occupancy()` reports packed area / page area; `indexOf(name)` and
  `name(i)` give you a stable name → index mapping for a loader.
- `pixels()` is RGBA8, transparent where nothing was packed, and top-down
  like the rest of the engine's image helpers.

## LightingSystem

2D lighting without normal maps. Lights are accumulated additively into an
offscreen lightmap that starts at the ambient color, then the lightmap is
**multiplied** over the already-drawn scene: white = unchanged, black = dark.

```cpp
forge::LightingSystem lights;
lights.init(vp.x, vp.y);                 // owns a viewport-sized FBO
lights.setAmbient(forge::Color::fromHex(0x202030FF));

forge::Light2D torch;
torch.pos = {120, 60};
torch.radius = 220;
torch.color = forge::Color::fromHex(0xFFD9A0FF);
torch.intensity = 1.0f;
torch.flickerAmount = 0.08f;             // deterministic, time-based
torch.flickerSpeed = 8.0f;
lights.addLight(torch);

// per frame, AFTER the world batch has ended:
lights.update(dt);
lights.render(batch, cam, vp);           // composites over the screen
```

- `render` begins and ends its own passes, so call it between batch passes
  (world pass ended, HUD pass not started).
- `target` (optional 4th argument) re-binds an offscreen framebuffer after the
  lightmap pass — required when the scene was rendered into an FBO (post-
  processing pipeline, `--shot` capture).
- Shadows are opt-in per light: `light.shadows = true` plus
  `addOccluder(a, b)` line segments. Shadow volumes are drawn as black
  polygons after the lights, so they also darken the ambient term.
- `flickerAmount`/`flickerSpeed`/`flickerPhase` are applied at draw time from
  the `update(dt)` clock; `Light2D::intensity` stays the authored value, so
  rebuilding the light list every frame is safe.
- `resize()` on window changes; `clearLights()` per frame if your lights move.
  `lightmapTexture()` exposes the raw lightmap for custom compositing.
- Cost is one extra FBO and one fullscreen multiply; the radial falloff
  texture is generated once (128²) and shared by every light.

## Post-processing and render targets

```cpp
forge::PostProcess fx;
fx.init(forge::PostProcess::Effect::Vignette);
fx.setVignette(0.6f, 0.7f);   // strength, radius
fx.setScanlines(0.15f);
fx.setTime(t);

// render the world into an offscreen target first
forge::Framebuffer scene;
scene.create(vp.x, vp.y);
scene.bind();
/* ...world batch pass... */
forge::Framebuffer::unbind(vp.x, vp.y);

forge::Texture2D view;
view.adoptRaw(scene.texture(), scene.width(), scene.height(), false);
fx.apply(view, vp);            // between batch passes
```

- `apply` must run **between** `SpriteBatch` passes: it binds its own program,
  VAO and texture, and disables blending/scissor. Re-`begin()` the batch after.
- `flipY` defaults to `false` because framebuffer textures have the same
  bottom-left origin as the target. Pass `true` only for a top-down image
  source (e.g. a decoded PNG used as a background).
- `FullscreenPass` is the raw version: `load(vert, frag)`, `setFloat/Vec2/Vec4/
  Int`, then `apply(src, viewport)`. Use it for custom effects (bloom, palette
  swap, screen shake warp); the built-in `PostProcess` is just a preset.
- `RenderTargetStack` pools framebuffers: `push(w, h)` binds a target and
  returns its `Texture2D&`, `pop(screenW, screenH)` restores the previous one
  (or the screen) and returns the popped texture. Use it instead of manually
  tracking nested FBOs.
- Draw the HUD **after** the post pass so UI text stays crisp — that is what
  `10_lantern` does.

## Skeletal animation

Bones + slots + keyframe tracks, loaded from JSON via `utils/json.hpp`.

```cpp
forge::Skeleton skel;
if (skel.loadJson("hero.skel.json")) {
    // Slot textures are resolved by name; the skeleton never owns them.
    skel.resolveTextures([&](const std::string& name) {
        return engine.assets().loadTexture("assets/" + name + ".png");
    });
    skel.play("walk");

    // per frame:
    skel.update(dt);                       // sample tracks, restore rest pose
    skel.draw(batch, heroPos, 0.0f, {1, 1}, /*flipX=*/facing < 0);
    if (debug) skel.debugDraw(batch, heroPos);
}
```

```cpp
bool loadJson(const std::string& path);
bool parseJson(const std::string& text, const std::string& sourceName = "<memory>");
bool valid() const;

int  addBone(const Bone&);  int findBone(const std::string& name) const;
int  addSlot(const Slot&);  int findSlot(const std::string& name) const;
bool setSlotTexture(const std::string& slotName, const Texture2D* tex);

int  addAnimation(const Animation&);  int findAnimation(const std::string&) const;
bool play(const std::string& name, bool restart = true);  void stop();
void setSpeed(float s);  bool playing() const;  const std::string& currentAnimation() const;

void update(float dt);      // advance + recompute world transforms
void applyPose();           // recompute transforms without advancing
void draw(SpriteBatch&, Vec2 origin, float rotation = 0, Vec2 scale = {1,1},
          bool flipX = false) const;
void debugDraw(SpriteBatch&, Vec2 origin, float rotation = 0, Vec2 scale = {1,1}) const;
```

- Bone hierarchy is parent-first; `update()` restores `restPos/restRotation/
  restScale` before sampling, so removing a track snaps back instead of
  accumulating. `applyPose()` is for building a pose without advancing time
  (e.g. an editor or a paused frame).
- Tracks interpolate translation, rotation and scale linearly between keys;
  times are seconds and loop with the clip duration (`Animation::loop`).
- Slots draw in `drawOrder`, each as one quad with the slot's UV rect. There is
  no mesh deformation (no free-form/skinned vertices) — that is a non-goal for
  the arcade-grade engine.
- `debugDraw` draws bone lines and joint dots; use it while authoring JSON.

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
