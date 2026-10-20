#include "forge/graphics/tilemap.hpp"
#include "forge/graphics/sprite_batch.hpp"
#include "forge/graphics/texture.hpp"
#include "forge/core/logger.hpp"
#include "forge/utils/json.hpp"
#include <fstream>
#include <sstream>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace forge {

namespace {

// Tiled GID flag bits.
constexpr uint32_t kFlipH = 0x80000000u;
constexpr uint32_t kFlipV = 0x40000000u;
constexpr uint32_t kFlipD = 0x20000000u; // diagonal (anti-diagonal)
constexpr uint32_t kGidMask = 0x1FFFFFFFu;

std::string dirOf(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

std::string joinPath(const std::string& dir, const std::string& rel) {
    if (rel.empty()) return rel;
    if (rel[0] == '/' || (rel.size() > 1 && rel[1] == ':')) return rel; // absolute
    return dir + rel;
}

} // namespace

void Tilemap::create(int w, int h, int tileW, int tileH) {
    w_ = w; h_ = h; tileW_ = tileW; tileH_ = tileH;
    layers_.clear();
    objects_.clear();
    originOffset_ = {0, 0};
    addLayer("main");
}

int Tilemap::addLayer(const std::string& name) {
    TileLayer l;
    l.name = name;
    l.width = w_; l.height = h_;
    l.tiles.assign((size_t)w_ * h_, 0);
    l.flags.assign((size_t)w_ * h_, 0);
    layers_.push_back(std::move(l));
    return (int)layers_.size() - 1;
}

bool Tilemap::loadCsv(const std::string& csv, int w, int h, int tileW, int tileH) {
    create(w, h, tileW, tileH);
    std::stringstream ss(csv);
    std::string cell;
    int i = 0;
    while (std::getline(ss, cell, ',') && i < w * h) {
        layers_[0].tiles[i++] = std::atoi(cell.c_str());
    }
    return i == w * h;
}

bool Tilemap::loadTiledJson(const std::string& path) {
    json::ParseError err;
    json::Value root = json::parseFile(path, &err);
    if (err.ok() && root.type() != json::Value::Type::Object) {
        err.message = "root is not an object";
    }
    if (!err.ok()) {
        FORGE_LOG_ERROR("Tilemap '%s': %s", path.c_str(), err.str().c_str());
        return false;
    }

    w_ = 0; h_ = 0; tileW_ = 16; tileH_ = 16;
    layers_.clear();
    tilesets_.clear();
    objects_.clear();
    tileset_ = nullptr;
    columns_ = tileCount_ = 0;
    originOffset_ = {0, 0};
    sourcePath_ = path;
    const std::string dir = dirOf(path);

    const json::Value& jtw = root["tilewidth"];
    const json::Value& jth = root["tileheight"];
    if (!jtw.isNumber() || !jth.isNumber()) {
        FORGE_LOG_ERROR("Tilemap '%s': missing tilewidth/tileheight", path.c_str());
        return false;
    }
    tileW_ = jtw.asInt();
    tileH_ = jth.asInt();
    if (tileW_ <= 0 || tileH_ <= 0) {
        FORGE_LOG_ERROR("Tilemap '%s': invalid tile size %dx%d", path.c_str(), tileW_, tileH_);
        return false;
    }

    const bool infinite = root["infinite"].asBool(false);
    w_ = root["width"].asInt(0);
    h_ = root["height"].asInt(0);
    if (!infinite && (w_ <= 0 || h_ <= 0)) {
        FORGE_LOG_ERROR("Tilemap '%s': invalid dimensions %dx%d", path.c_str(), w_, h_);
        return false;
    }

    // ---- tilesets (all of them; firstgid may be non-contiguous) ----
    const json::Value& sets = root["tilesets"];
    for (size_t i = 0; i < sets.size(); ++i) {
        const json::Value& ts = sets.at(i);
        TilesetRef ref;
        ref.firstId = ts["firstgid"].asInt(1);
        ref.columns = ts["columns"].asInt(0);
        ref.tileCount = ts["tilecount"].asInt(0);
        ref.tileW = ts["tilewidth"].asInt(tileW_);
        ref.tileH = ts["tileheight"].asInt(tileH_);
        ref.spacing = ts["spacing"].asInt(0);
        ref.margin = ts["margin"].asInt(0);
        ref.imagePath = ts["image"].asString();
        // External tileset (.tsj): pull the image out of it.
        if (ref.imagePath.empty() && ts["source"].isString()) {
            json::ParseError terr;
            json::Value ext = json::parseFile(joinPath(dir, ts["source"].asString()), &terr);
            if (terr.ok()) {
                ref.imagePath = ext["image"].asString();
                ref.columns = ext["columns"].asInt(ref.columns);
                ref.tileCount = ext["tilecount"].asInt(ref.tileCount);
                ref.tileW = ext["tilewidth"].asInt(ref.tileW);
                ref.tileH = ext["tileheight"].asInt(ref.tileH);
                ref.spacing = ext["spacing"].asInt(ref.spacing);
                ref.margin = ext["margin"].asInt(ref.margin);
            } else {
                FORGE_LOG_WARN("Tilemap '%s': cannot read tileset '%s': %s", path.c_str(),
                               ts["source"].asString().c_str(), terr.str().c_str());
            }
        }
        if (ref.tileW != tileW_ || ref.tileH != tileH_) {
            FORGE_LOG_ERROR("Tilemap '%s': tileset tile size %dx%d != map tile size %dx%d",
                            path.c_str(), ref.tileW, ref.tileH, tileW_, tileH_);
            return false;
        }
        if (ref.columns <= 0 && ref.tileCount > 0) {
            // Derive from the image once it is loaded; keep 0 for now.
        }
        ref.imageResolvedPath = joinPath(dir, ref.imagePath);
        tilesets_.push_back(std::move(ref));
    }
    // Legacy single-tileset accessors point at the first image tileset.
    for (size_t i = 0; i < tilesets_.size(); ++i) {
        if (!tilesets_[i].imagePath.empty()) {
            tilesetPath_ = tilesets_[i].imagePath;
            columns_ = tilesets_[i].columns;
            tileCount_ = tilesets_[i].tileCount;
            break;
        }
    }

    // ---- layers ----
    const json::Value& jlayers = root["layers"];
    if (jlayers.size() == 0) {
        FORGE_LOG_ERROR("Tilemap '%s': no layers", path.c_str());
        return false;
    }

    // Infinite maps: compute dense bounds from every chunk across all layers.
    int minX = 0, minY = 0, maxX = w_ > 0 ? w_ - 1 : 0, maxY = h_ > 0 ? h_ - 1 : 0;
    bool anyChunk = false;
    if (infinite) {
        for (size_t li = 0; li < jlayers.size(); ++li) {
            const json::Value& lj = jlayers.at(li);
            if (lj["type"].asString() != "tilelayer") continue;
            const json::Value& chunks = lj["chunks"];
            for (size_t ci = 0; ci < chunks.size(); ++ci) {
                const json::Value& c = chunks.at(ci);
                int cx = c["x"].asInt(), cy = c["y"].asInt();
                int cw = c["width"].asInt(), ch = c["height"].asInt();
                if (!anyChunk) {
                    minX = cx; minY = cy; maxX = cx + cw - 1; maxY = cy + ch - 1;
                    anyChunk = true;
                } else {
                    minX = std::min(minX, cx);
                    minY = std::min(minY, cy);
                    maxX = std::max(maxX, cx + cw - 1);
                    maxY = std::max(maxY, cy + ch - 1);
                }
            }
        }
        if (!anyChunk) {
            FORGE_LOG_ERROR("Tilemap '%s': infinite map without chunks", path.c_str());
            return false;
        }
        w_ = maxX - minX + 1;
        h_ = maxY - minY + 1;
        // Tiled chunk Y is top-down. After flipping, the map's bottom row is
        // Tiled's maxY. originOffset_ keeps world (0,0) at the map's
        // bottom-left so existing game code (origin at tile 0,0) is unchanged.
        originOffset_ = {(float)minX * tileW_, 0.0f};
    }

    for (size_t li = 0; li < jlayers.size(); ++li) {
        const json::Value& lj = jlayers.at(li);
        const std::string type = lj["type"].asString();
        if (type == "objectgroup") {
            const json::Value& objs = lj["objects"];
            for (size_t oi = 0; oi < objs.size(); ++oi) {
                const json::Value& o = objs.at(oi);
                TilemapObject obj;
                obj.name = o["name"].asString();
                obj.type = o["type"].asString();
                if (obj.type.empty()) obj.type = o["class"].asString();
                const bool isPoint = o["point"].asBool(false);
                float x = o["x"].asFloat(0), y = o["y"].asFloat(0);
                float ow = o["width"].asFloat(0), oh = o["height"].asFloat(0);
                obj.point = isPoint;
                // Tiled object Y is the top edge in a top-down map: convert to
                // a Y-up world rect using the map's pixel height.
                float mapH = (float)h_ * tileH_;
                obj.rect = {x, mapH - (y + oh), ow, oh};
                objects_.push_back(std::move(obj));
            }
            continue;
        }
        if (type != "tilelayer") continue;

        TileLayer l;
        l.name = lj["name"].asString("layer");
        l.width = w_;
        l.height = h_;
        l.visible = lj["visible"].asBool(true);
        l.opacity = lj["opacity"].asFloat(1.0f);
        l.offset = {lj["offsetx"].asFloat(0), lj["offsety"].asFloat(0)};
        l.parallax = {lj["parallaxx"].asFloat(1.0f), lj["parallaxy"].asFloat(1.0f)};
        l.tiles.assign((size_t)w_ * h_, 0);
        l.flags.assign((size_t)w_ * h_, 0);

        auto place = [&](int sx, int sy, uint32_t raw) {
            int local = 0;
            uint8_t flags = 0;
            uint32_t gid = raw & kGidMask;
            if (gid != 0) {
                // Find the tileset whose firstgid range contains this gid.
                int tsIndex = -1;
                for (int ti = (int)tilesets_.size() - 1; ti >= 0; --ti) {
                    if ((int)gid >= tilesets_[(size_t)ti].firstId) { tsIndex = ti; break; }
                }
                if (tsIndex < 0) tsIndex = 0;
                int localInSet = (int)gid - (tilesets_.empty() ? 1 : tilesets_[(size_t)tsIndex].firstId) + 1;
                // Encode the tileset index in the high byte of the local id so
                // tileUv() can resolve it without a parallel array.
                local = (tsIndex << 16) | (localInSet & 0xFFFF);
                if (raw & kFlipH) flags |= 1;
                if (raw & kFlipV) flags |= 2;
                if (raw & kFlipD) flags |= 4;
            }
            // Y-up conversion: Tiled rows are top-down.
            int dy = (infinite ? maxY : h_ - 1) - sy;
            int dx = sx - (infinite ? minX : 0);
            if (dx < 0 || dy < 0 || dx >= w_ || dy >= h_) return;
            l.tiles[(size_t)dy * w_ + dx] = local;
            l.flags[(size_t)dy * w_ + dx] = flags;
        };

        const json::Value& chunks = lj["chunks"];
        if (infinite && chunks.size() > 0) {
            for (size_t ci = 0; ci < chunks.size(); ++ci) {
                const json::Value& c = chunks.at(ci);
                int cx = c["x"].asInt(), cy = c["y"].asInt();
                int cw = c["width"].asInt(), ch = c["height"].asInt();
                (void)ch;
                const json::Value& data = c["data"];
                for (size_t i = 0; i < data.size(); ++i) {
                    int sx = cx + (int)(i % (size_t)cw);
                    int sy = cy + (int)(i / (size_t)cw);
                    place(sx, sy, (uint32_t)data.at(i).asInt64(0));
                }
            }
        } else {
            const json::Value& data = lj["data"];
            for (size_t i = 0; i < data.size(); ++i) {
                int sx = (int)(i % (size_t)w_);
                int sy = (int)(i / (size_t)w_);
                place(sx, sy, (uint32_t)data.at(i).asInt64(0));
            }
        }
        layers_.push_back(std::move(l));
    }

    FORGE_LOG_INFO("Tilemap '%s': %dx%d, %d layers, %d tilesets%s", path.c_str(), w_, h_,
                   (int)layers_.size(), (int)tilesets_.size(),
                   infinite ? " (infinite, expanded)" : "");
    return true;
}

int Tilemap::loadTilesets() {
    int loaded = 0;
    for (auto& ts : tilesets_) {
        if (!ts.texture && !ts.imageResolvedPath.empty()) {
            auto tex = std::make_unique<Texture2D>();
            if (tex->loadFile(ts.imageResolvedPath)) {
                if (ts.columns <= 0) {
                    ts.columns = std::max(1, tex->width() / (tileW_ + ts.spacing));
                }
                if (ts.tileCount <= 0) ts.tileCount = ts.columns * (tex->height() / (tileH_ + ts.spacing));
                ts.texture = tex.get();
                ownedTextures_.push_back(std::move(tex));
                ++loaded;
            } else {
                FORGE_LOG_WARN("Tilemap: tileset image failed: %s", ts.imagePath.c_str());
            }
        } else if (ts.texture) {
            ++loaded;
        }
    }
    // Keep the legacy pointer in sync.
    for (auto& ts : tilesets_) {
        if (ts.texture) { tileset_ = ts.texture; columns_ = ts.columns; tileCount_ = ts.tileCount; break; }
    }
    return loaded;
}

void Tilemap::setTilesetTexture(int i, const Texture2D* tex) {
    if (i < 0 || i >= (int)tilesets_.size()) return;
    tilesets_[(size_t)i].texture = tex;
    if (i == 0) { tileset_ = tex; columns_ = tilesets_[0].columns; tileCount_ = tilesets_[0].tileCount; }
}

int Tilemap::tilesetFor(int localId) const {
    int idx = (localId >> 16) & 0xFFFF;
    if (idx < (int)tilesets_.size()) return idx;
    return tilesets_.empty() ? -1 : 0;
}

void Tilemap::setTileset(const Texture2D* tex, int columns, int tileCount) {
    tileset_ = tex;
    columns_ = columns;
    tileCount_ = tileCount;
    if (tilesets_.empty()) tilesets_.emplace_back();
    tilesets_[0].texture = tex;
    tilesets_[0].columns = columns;
    tilesets_[0].tileCount = tileCount;
    tilesets_[0].tileW = tileW_;
    tilesets_[0].tileH = tileH_;
    tilesets_[0].firstId = 1;
}

void Tilemap::setTile(int layer, int x, int y, int id) {
    if (layer < 0 || layer >= (int)layers_.size()) return;
    TileLayer& l = layers_[(size_t)layer];
    if (x < 0 || y < 0 || x >= l.width || y >= l.height) return;
    l.tiles[(size_t)y * l.width + x] = id;
}

void Tilemap::setTileFlags(int layer, int x, int y, uint8_t flags) {
    if (layer < 0 || layer >= (int)layers_.size()) return;
    TileLayer& l = layers_[(size_t)layer];
    if (x < 0 || y < 0 || x >= l.width || y >= l.height) return;
    if (l.flags.size() < l.tiles.size()) l.flags.assign(l.tiles.size(), 0);
    l.flags[(size_t)y * l.width + x] = flags;
}

int Tilemap::tile(int layer, int x, int y) const {
    if (layer < 0 || layer >= (int)layers_.size()) return 0;
    const TileLayer& l = layers_[(size_t)layer];
    if (x < 0 || y < 0 || x >= l.width || y >= l.height) return 0;
    return l.tiles[(size_t)y * l.width + x] & 0xFFFF;
}

int Tilemap::tilesetIndex(int layer, int x, int y) const {
    if (layer < 0 || layer >= (int)layers_.size()) return -1;
    const TileLayer& l = layers_[(size_t)layer];
    if (x < 0 || y < 0 || x >= l.width || y >= l.height) return -1;
    int id = l.tiles[(size_t)y * l.width + x];
    if (id == 0) return -1;
    int idx = (id >> 16) & 0xFFFF;
    return idx < (int)tilesets_.size() ? idx : -1;
}

uint8_t Tilemap::tileFlags(int layer, int x, int y) const {
    if (layer < 0 || layer >= (int)layers_.size()) return 0;
    const TileLayer& l = layers_[(size_t)layer];
    if (x < 0 || y < 0 || x >= l.width || y >= l.height) return 0;
    if (l.flags.size() < l.tiles.size()) return 0;
    return l.flags[(size_t)y * l.width + x];
}

void Tilemap::setSolid(int tileId, bool solid) {
    if ((int)solid_.size() <= tileId) solid_.resize(tileId + 1, 0);
    solid_[tileId] = solid ? 1 : 0;
}

bool Tilemap::isSolid(int tileId) const {
    if (tileId < 0 || tileId >= (int)solid_.size()) return false;
    return solid_[tileId] != 0;
}

void Tilemap::setSlope(int tileId, float leftHeight, float rightHeight) {
    if (tileId < 0) return;
    if ((int)slopes_.size() <= tileId) slopes_.resize((size_t)tileId + 1);
    TileSlope s;
    s.enabled = true;
    s.left = clamp01(leftHeight);
    s.right = clamp01(rightHeight);
    slopes_[(size_t)tileId] = s;
    // A slope is a surface, so it must be solid to take part in collision.
    setSolid(tileId, true);
}

void Tilemap::clearSlope(int tileId) {
    if (tileId < 0 || tileId >= (int)slopes_.size()) return;
    slopes_[(size_t)tileId] = TileSlope{};
}

TileSlope Tilemap::slope(int tileId) const {
    if (tileId < 0 || tileId >= (int)slopes_.size()) return TileSlope{};
    return slopes_[(size_t)tileId];
}

float Tilemap::slopeSurfaceHeight(int tileId, float t) const {
    TileSlope s = slope(tileId);
    if (!s.enabled) return (float)tileH_;
    t = clamp01(t);
    return (s.left + (s.right - s.left) * t) * (float)tileH_;
}

float Tilemap::surfaceYAt(int layer, int x, int y, float worldX) const {
    if (layer < 0 || layer >= (int)layers_.size()) return 0.0f;
    const TileLayer& l = layers_[(size_t)layer];
    if (x < 0 || y < 0 || x >= l.width || y >= l.height) return 0.0f;
    int id = l.tiles[(size_t)y * l.width + x] & 0xFFFF;
    float tileX = originOffset_.x + (float)x * tileW_;
    float tileY = originOffset_.y + (float)y * tileH_;
    float t = tileW_ > 0 ? (worldX - tileX) / (float)tileW_ : 0.0f;
    return tileY + slopeSurfaceHeight(id, t);
}

void Tilemap::setAutoRule(const std::vector<int>& byMask) {
    autoRule_ = byMask;
    autoRule_.resize(256, 0);
}

std::vector<int> Tilemap::buildAutoRule(int baseId, int cornersId, int edgesId, int innerId) {
    // Mask bits: N, NE, E, SE, S, SW, W, NW (bit 0 = N, clockwise).
    std::vector<int> rule(256, baseId);
    for (int m = 0; m < 256; ++m) {
        int count = 0;
        for (int b = 0; b < 8; ++b) count += (m >> b) & 1;
        if (count == 0) rule[(size_t)m] = baseId;
        else if (count <= 2) rule[(size_t)m] = edgesId ? edgesId : baseId;
        else if (count <= 6) rule[(size_t)m] = cornersId ? cornersId : baseId;
        else rule[(size_t)m] = innerId ? innerId : baseId;
    }
    return rule;
}

void Tilemap::applyAutotile(int layerIndex) {
    if (autoRule_.size() != 256) return;
    if (layerIndex < 0 || layerIndex >= (int)layers_.size()) return;
    TileLayer& l = layers_[(size_t)layerIndex];
    std::vector<int> src = l.tiles;
    auto solidAt = [&](int x, int y) -> bool {
        if (x < 0 || y < 0 || x >= l.width || y >= l.height) return false;
        return src[(size_t)y * l.width + x] != 0;
    };
    for (int y = 0; y < l.height; ++y)
        for (int x = 0; x < l.width; ++x) {
            if (!solidAt(x, y)) continue;
            int mask = 0;
            // N, NE, E, SE, S, SW, W, NW
            const int dx[8] = {0, 1, 1, 1, 0, -1, -1, -1};
            const int dy[8] = {1, 1, 0, -1, -1, -1, 0, 1};
            for (int b = 0; b < 8; ++b)
                if (solidAt(x + dx[b], y + dy[b])) mask |= (1 << b);
            int id = autoRule_[(size_t)mask];
            if (id > 0) l.tiles[(size_t)y * l.width + x] = id;
        }
}

std::vector<Rect> Tilemap::solidsInRect(Rect r) const {
    std::vector<Rect> out;
    if (layers_.empty()) return out;
    // floor (not truncation): negative coords must round down
    int x0 = (int)std::floor((r.x - originOffset_.x) / tileW_) - 1;
    int x1 = (int)std::floor((r.x + r.w - originOffset_.x) / tileW_) + 1;
    int y0 = (int)std::floor((r.y - originOffset_.y) / tileH_) - 1;
    int y1 = (int)std::floor((r.y + r.h - originOffset_.y) / tileH_) + 1;
    // Visibility is a render concern: hiding a layer must NOT silently remove
    // its solids (toggling a "collision" layer's visibility is a drawing
    // decision, not a physics one). Delete/skip the layer explicitly instead.
    for (auto& l : layers_) {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                if (x < 0 || y < 0 || x >= l.width || y >= l.height) continue;
                int id = l.tiles[(size_t)y * l.width + x];
                if (id != 0 && isSolid(id & 0xFFFF))
                    out.push_back({originOffset_.x + (float)x * tileW_,
                                   originOffset_.y + (float)y * tileH_,
                                   (float)tileW_, (float)tileH_});
            }
    }
    return out;
}

bool Tilemap::collideRect(Rect r, Vec2* resolve) const {
    auto solids = solidsInRect(r);
    Vec2 total{0, 0};
    bool hit = false;
    for (auto& s : solids) {
        if (!r.overlaps(s)) continue;
        hit = true;
        // Which tile is this? solidsInRect only returns world rects, so map
        // back through the origin offset to look up slope data.
        int tx = (int)std::floor((s.x - originOffset_.x) / tileW_);
        int ty = (int)std::floor((s.y - originOffset_.y) / tileH_);
        int localId = 0;
        if (!layers_.empty()) {
            const TileLayer& l0 = layers_.front();
            if (tx >= 0 && ty >= 0 && tx < l0.width && ty < l0.height)
                localId = l0.tiles[(size_t)ty * l0.width + tx] & 0xFFFF;
        }
        if (isSlope(localId)) {
            // Slope: resolve vertically only, against the surface under the
            // rect's horizontal center. Walking up from below is allowed
            // (one-way) so ramps never trap the player inside them.
            float cx = std::clamp(r.x + r.w * 0.5f, s.x, s.x + s.w);
            float t = s.w > 0 ? (cx - s.x) / s.w : 0.0f;
            float surface = s.y + slopeSurfaceHeight(localId, t);
            if (r.y < surface) {
                total.y += surface - r.y;
                r.y = surface;
            }
            continue;
        }
        // push out along smallest axis
        float dx1 = (s.x + s.w) - r.x;   // push +x
        float dx2 = r.x + r.w - s.x;     // push -x (magnitude)
        float dy1 = (s.y + s.h) - r.y;
        float dy2 = r.y + r.h - s.y;
        float mx = std::min(dx1, dx2), my = std::min(dy1, dy2);
        if (mx < my) {
            total.x += (dx1 < dx2 ? mx : -mx);
            r.x += (dx1 < dx2 ? mx : -mx);
        } else {
            total.y += (dy1 < dy2 ? my : -my);
            r.y += (dy1 < dy2 ? my : -my);
        }
    }
    if (resolve) *resolve = total;
    return hit;
}

Tilemap::TileUv Tilemap::tileUv(int localId) const {
    TileUv out;
    out.uv = {0, 0, 1, 1};
    if (localId <= 0) return out;

    int tsIndex = (localId >> 16) & 0xFFFF;
    int idInSet = localId & 0xFFFF;
    const TilesetRef* ts = nullptr;
    if (tsIndex < (int)tilesets_.size() && tilesets_[(size_t)tsIndex].texture) {
        ts = &tilesets_[(size_t)tsIndex];
    } else if (tileset_) {
        // Legacy path: plain 1-based id, single tileset.
        ts = tilesets_.empty() ? nullptr : &tilesets_[0];
        idInSet = localId;
    }
    if (!ts || !ts->texture) return out;

    int cols = ts->columns > 0 ? ts->columns : 1;
    int idx = idInSet - 1;
    if (idx < 0) return out;
    int cx = idx % cols;
    int cy = idx / cols;
    float tw = (float)ts->texture->width(), th = (float)ts->texture->height();
    int margin = ts->margin, spacing = ts->spacing;
    float px = (float)(margin + cx * (tileW_ + spacing));
    float py = (float)(margin + cy * (tileH_ + spacing));
    // Textures load flipped (flipY=true) so GL row 0 is image top.
    // Tiled rows are top-down: flip V back.
    out.uv = {px / tw, 1.0f - (py + tileH_) / th, tileW_ / tw, tileH_ / th};
    out.tileset = tsIndex;
    return out;
}

void Tilemap::draw(SpriteBatch& batch, Vec2 origin, Rect view) const {
    if (layers_.empty()) return;
    const Vec2 base = origin + originOffset_;

    for (const auto& l : layers_) {
        if (!l.visible) continue;
        // Parallax: the layer moves with the camera by `parallax`; a factor of
        // 0 pins it to the world origin, 1 is normal.
        Vec2 camCenter = view.center();
        Vec2 off = l.offset;
        Vec2 par{off.x + camCenter.x * (l.parallax.x - 1.0f),
                 off.y + camCenter.y * (l.parallax.y - 1.0f)};
        Vec2 layerOrigin = base + par;

        int x0 = (int)std::floor((view.x - layerOrigin.x) / tileW_) - 1;
        int x1 = (int)std::floor((view.x + view.w - layerOrigin.x) / tileW_) + 1;
        int y0 = (int)std::floor((view.y - layerOrigin.y) / tileH_) - 1;
        int y1 = (int)std::floor((view.y + view.h - layerOrigin.y) / tileH_) + 1;
        Color tint{1, 1, 1, l.opacity};

        for (int y = std::max(y0, 0); y <= std::min(y1, l.height - 1); ++y)
            for (int x = std::max(x0, 0); x <= std::min(x1, l.width - 1); ++x) {
                size_t ti = (size_t)y * l.width + x;
                int id = l.tiles[ti];
                if (id <= 0) continue;
                TileUv uv = tileUv(id);
                int tsIdx = uv.tileset;
                const Texture2D* tex = nullptr;
                if (tsIdx >= 0 && tsIdx < (int)tilesets_.size()) tex = tilesets_[(size_t)tsIdx].texture;
                if (!tex) tex = tileset_;
                if (!tex || !tex->valid()) continue;

                SpriteDraw d;
                d.tex = tex;
                d.pos = {layerOrigin.x + x * tileW_ + tileW_ * 0.5f,
                         layerOrigin.y + y * tileH_ + tileH_ * 0.5f};
                d.size = {(float)tileW_, (float)tileH_};
                d.src = uv.uv;
                d.color = tint;
                uint8_t flags = (l.flags.size() > ti) ? l.flags[ti] : 0;
                FlipTransform f = decodeFlip(flags);
                d.rotation = f.rotation;
                d.flipX = f.flipX;
                d.flipY = f.flipY;
                batch.draw(d);
            }
    }
}

// Tiled's 8 flip combinations: bits are H (1), V (2), D=diagonal (4).
// Non-diagonal cases are plain flips. The diagonal case is a transpose; with
// a square tile a 90-degree rotation plus the transposed flips reproduces it
// exactly. Order matters: Tiled applies diagonal first, then H/V.
Tilemap::FlipTransform Tilemap::decodeFlip(uint8_t flags) {
    FlipTransform f;
    const bool h = (flags & 1) != 0, v = (flags & 2) != 0, d = (flags & 4) != 0;
    if (!d) {
        f.flipX = h;
        f.flipY = v;
    } else {
        f.rotation = PI * 0.5f;
        f.flipX = !h;
        f.flipY = v;
    }
    return f;
}

void Tilemap::drawAll(SpriteBatch& batch, Vec2 origin) const {
    draw(batch, origin, {origin.x, origin.y, (float)w_ * tileW_, (float)h_ * tileH_});
}

} // namespace forge
