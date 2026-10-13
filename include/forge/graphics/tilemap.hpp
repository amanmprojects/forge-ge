#pragma once
// Tilemap: CSV / Tiled-JSON tile layers + renderer with view culling.
// Loads Tiled (tiledapp.com) exports: JSON with `layers[].data`,
// `tilewidth/tileheight`, external tileset image path in `tilesets[].image`.

#include "forge/math/math.hpp"
#include "forge/graphics/texture.hpp"
#include <string>
#include <vector>

namespace forge {

class SpriteBatch;

struct TileLayer {
    std::string name;
    int width = 0, height = 0;
    std::vector<int> tiles; // row-major, 0 = empty, otherwise 1-based tile id
    bool visible = true;
    float opacity = 1.0f;
};

class Tilemap {
public:
    Tilemap() = default;

    // Coordinate convention: Y-up, tile (0,0) is the BOTTOM-left.
    // - create/setTile/tile use Y-up directly.
    // - loadCsv: row 0 of the CSV is the BOTTOM row (first values = y 0).
    // - loadTiledJson: Tiled data is top-down; rows are flipped on load so
    //   Tiled's top row becomes y = height-1. Only the first tileset is used
    //   (firstgid remapped); tile flip flags are masked (rendered unflipped).
    void create(int w, int h, int tileW, int tileH);
    bool loadTiledJson(const std::string& path);   // needs tileset texture set after
    bool loadCsv(const std::string& csv, int w, int h, int tileW, int tileH);

    void setTileset(const Texture2D* tex, int columns, int tileCount);
    void setTile(int layer, int x, int y, int id);
    int tile(int layer, int x, int y) const;

    int addLayer(const std::string& name);
    int layerCount() const { return (int)layers_.size(); }

    int width() const { return w_; }
    int height() const { return h_; }
    int tileW() const { return tileW_; }
    int tileH() const { return tileH_; }
    Vec2 worldSize() const { return {(float)w_ * tileW_, (float)h_ * tileH_}; }

    // Solid tiles for physics: mark tile ids solid, query AABBs in rect
    void setSolid(int tileId, bool solid = true);
    bool isSolid(int tileId) const;
    std::vector<Rect> solidsInRect(Rect r) const; // world-space AABBs
    bool collideRect(Rect r, Vec2* resolve = nullptr) const;

    // Culls to camera view rect; origin = world pos of tile (0,0) bottom-left
    void draw(SpriteBatch& batch, Vec2 origin, Rect view) const;
    void drawAll(SpriteBatch& batch, Vec2 origin) const;

    const std::string& tilesetPath() const { return tilesetPath_; }

private:
    Rect tileUv(int id) const;

    int w_ = 0, h_ = 0, tileW_ = 16, tileH_ = 16;
    std::vector<TileLayer> layers_;
    const Texture2D* tileset_ = nullptr;
    int columns_ = 0, tileCount_ = 0;
    std::vector<char> solid_;
    std::string tilesetPath_;
};

} // namespace forge
