#pragma once
// Tilemap: CSV / Tiled-JSON tile layers + renderer with view culling.
//
// Coordinate convention: Y-up, tile (0,0) is the BOTTOM-left.
// - create/setTile/tile use Y-up directly.
// - loadCsv: row 0 of the CSV is the BOTTOM row (first values = y 0).
// - loadTiledJson: Tiled data is top-down; rows are flipped on load so
//   Tiled's top row becomes y = height-1.
//
// Supported Tiled features:
// - multiple tilesets (`firstgid` remapped per tile, `tilesetFor(id)`)
// - per-tile flip flags: horizontal, vertical, diagonal (anti-diagonal)
// - infinite maps: chunked data is expanded into dense bounds; the map keeps
//   an origin offset so negative chunk coordinates still render correctly
// - object layers: rectangles and points exposed as TilemapObject
// - per-layer offset, opacity, visibility and parallax factor
//
// Slope tiles (opt-in, per tile id): a solid tile can expose a sloped top
// surface instead of a full box. `leftHeight`/`rightHeight` are fractions of
// the tile height (0 = tile bottom, 1 = tile top), so 45 degrees is
// (0, 1) or (1, 0) and arbitrary ramps are (0.25, 0.75) and so on. Slopes are
// resolved vertically by collideRect(); everything else treats them as boxes.
//
// Not supported: isometric/hexagonal maps, tile animations, image layers.

#include "forge/math/math.hpp"
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

namespace forge {

class SpriteBatch;
class Texture2D;

struct TileLayer {
    std::string name;
    int width = 0, height = 0;
    std::vector<int> tiles;     // 1-based tile id within its tileset
    std::vector<uint8_t> flags; // bit 0 = flipH, 1 = flipV, 2 = diagonal
    bool visible = true;
    float opacity = 1.0f;
    Vec2 offset{0, 0};      // world offset in pixels
    Vec2 parallax{1, 1};    // 1 = moves with the camera, 0 = fixed
};

struct TileSlope {
    bool enabled = false;
    float left = 0.0f;  // 0..1 of tile height at the left edge
    float right = 1.0f; // 0..1 of tile height at the right edge
};

struct TilemapObject {
    std::string name;
    std::string type;       // Tiled "type"/"class"
    Rect rect{0, 0, 0, 0};  // world-space, Y-up
    bool point = false;
};

struct TilesetRef {
    const Texture2D* texture = nullptr;
    int firstId = 1;   // 1-based local id of the first tile
    int columns = 0;
    int tileCount = 0;
    int tileW = 0, tileH = 0;
    int spacing = 0, margin = 0;
    std::string imagePath;         // as written in the map file
    std::string imageResolvedPath; // map-dir-relative, used by loadTilesets()
};

class Tilemap {
public:
    Tilemap() = default;

    void create(int w, int h, int tileW, int tileH);
    // Loads a Tiled JSON map. Tileset images are NOT loaded here: after this
    // returns, call loadTilesets() or set each tileset's texture yourself
    // (see tilesets()/setTilesetTexture()).
    bool loadTiledJson(const std::string& path);
    bool loadCsv(const std::string& csv, int w, int h, int tileW, int tileH);

    // Loads every tileset image referenced by loadTiledJson, resolving paths
    // relative to the map file. Returns the number loaded.
    int loadTilesets();
    // Assign a texture to tileset index `i` (after loading it yourself).
    void setTilesetTexture(int i, const Texture2D* tex);
    const std::vector<TilesetRef>& tilesets() const { return tilesets_; }
    int tilesetFor(int localId) const; // index into tilesets(), -1 if none

    void setTileset(const Texture2D* tex, int columns, int tileCount);

    void setTile(int layer, int x, int y, int id);
    void setTileFlags(int layer, int x, int y, uint8_t flags);
    // Returns the id within its tileset (1-based). Use tilesetIndex() to know
    // which tileset it belongs to.
    int tile(int layer, int x, int y) const;
    int tilesetIndex(int layer, int x, int y) const;
    uint8_t tileFlags(int layer, int x, int y) const;

    int addLayer(const std::string& name);
    int layerCount() const { return (int)layers_.size(); }
    TileLayer& layer(int i) { return layers_[(size_t)i]; }
    const TileLayer& layer(int i) const { return layers_[(size_t)i]; }

    int width() const { return w_; }
    int height() const { return h_; }
    int tileW() const { return tileW_; }
    int tileH() const { return tileH_; }
    Vec2 worldSize() const { return {(float)w_ * tileW_, (float)h_ * tileH_}; }
    // World position of tile (0,0)'s bottom-left for infinite maps (may be
    // negative). Zero for finite maps.
    Vec2 originOffset() const { return originOffset_; }

    // Solid tiles for physics: mark tile ids solid, query AABBs in rect.
    // The id is the within-tileset id (what tile() returns), so the same rule
    // applies to every tileset.
    const std::vector<TilemapObject>& objects() const { return objects_; }
    void setSolid(int tileId, bool solid = true);
    bool isSolid(int tileId) const;
    // Slope surface for a tile id. Passing left == right == 1 keeps a flat
    // (full-box) tile but still marks it as a slope surface.
    void setSlope(int tileId, float leftHeight, float rightHeight);
    void clearSlope(int tileId);
    TileSlope slope(int tileId) const;
    bool isSlope(int tileId) const { return slope(tileId).enabled; }
    // Height of the slope surface at normalized x (0..1 across the tile),
    // in pixels above the tile's bottom edge. Non-slopes return tileH().
    float slopeSurfaceHeight(int tileId, float t) const;
    // World-space Y of the walkable surface under `worldX` inside tile (x,y),
    // or the tile bottom when the tile is not a slope.
    float surfaceYAt(int layer, int x, int y, float worldX) const;
    std::vector<Rect> solidsInRect(Rect r) const; // world-space AABBs
    bool collideRect(Rect r, Vec2* resolve = nullptr) const;

    // Culls to camera view rect; origin = world pos of tile (0,0) bottom-left
    void draw(SpriteBatch& batch, Vec2 origin, Rect view) const;
    void drawAll(SpriteBatch& batch, Vec2 origin) const;

    const std::string& tilesetPath() const { return tilesetPath_; }
    const std::string& sourcePath() const { return sourcePath_; }

    // ---- autotiling ----
    // Rule table: for each of the 256 8-neighbour masks, the tile id to place
    // (0 = leave empty). See buildAutoRule for a simple "any neighbour" rule.
    void setAutoRule(const std::vector<int>& byMask);
    // Applies the rule to every cell of `layer` based on non-empty neighbours.
    void applyAutotile(int layer);
    static std::vector<int> buildAutoRule(int baseId, int cornersId = 0,
                                          int edgesId = 0, int innerId = 0);

private:
    struct TileUv { Rect uv; int tileset = 0; };
    TileUv tileUv(int localId) const;
    struct FlipTransform { float rotation = 0; bool flipX = false, flipY = false; };
    static FlipTransform decodeFlip(uint8_t flags);

    int w_ = 0, h_ = 0, tileW_ = 16, tileH_ = 16;
    std::vector<TileLayer> layers_;
    std::vector<TilesetRef> tilesets_;
    std::vector<TilemapObject> objects_;
    const Texture2D* tileset_ = nullptr; // legacy single-tileset API
    int columns_ = 0, tileCount_ = 0;
    std::vector<char> solid_;
    std::vector<TileSlope> slopes_; // indexed by within-tileset tile id
    std::vector<int> autoRule_;
    std::string tilesetPath_;
    std::string sourcePath_;
    Vec2 originOffset_{0, 0};
    std::vector<std::unique_ptr<Texture2D>> ownedTextures_;
};

} // namespace forge
