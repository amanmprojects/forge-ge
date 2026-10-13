#include "forge/graphics/tilemap.hpp"
#include "forge/graphics/sprite_batch.hpp"
#include "forge/core/logger.hpp"
#include <fstream>
#include <sstream>
#include <cctype>
#include <cstring>

// ---- Minimal JSON parser (enough for Tiled exports) ----
namespace {
struct JVal {
    enum T { Nul, Num, Str, Bool, Arr, Obj } type = Nul;
    double num = 0;
    std::string str;
    bool boolean = false;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;

    const JVal* find(const char* key) const {
        if (type != Obj) return nullptr;
        for (auto& kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    double numOr(double d) const { return type == Num ? num : d; }
    const char* strOr(const char* d) const { return type == Str ? str.c_str() : d; }
    bool boolOr(bool d) const { return type == Bool ? boolean : d; }
};

struct Parser {
    const char* p;
    void skip() { while (*p && (isspace((unsigned char)*p))) ++p; }
    JVal parse() { skip(); return parseVal(); }
    JVal parseVal() {
        skip();
        if (*p == '{') return parseObj();
        if (*p == '[') return parseArr();
        if (*p == '"') { JVal v; v.type = JVal::Str; v.str = parseStr(); return v; }
        if (!strncmp(p, "true", 4)) { p += 4; JVal v; v.type = JVal::Bool; v.boolean = true; return v; }
        if (!strncmp(p, "false", 5)) { p += 5; JVal v; v.type = JVal::Bool; v.boolean = false; return v; }
        if (!strncmp(p, "null", 4)) { p += 4; return JVal{}; }
        JVal v; v.type = JVal::Num; v.num = parseNum(); return v;
    }
    JVal parseObj() {
        JVal v; v.type = JVal::Obj; ++p; // {
        skip();
        if (*p == '}') { ++p; return v; }
        while (*p) {
            skip();
            if (*p != '"') break;
            std::string k = parseStr();
            skip();
            if (*p == ':') ++p;
            v.obj.emplace_back(k, parseVal());
            skip();
            if (*p == ',') { ++p; continue; }
            if (*p == '}') { ++p; break; }
            break;
        }
        return v;
    }
    JVal parseArr() {
        JVal v; v.type = JVal::Arr; ++p; // [
        skip();
        if (*p == ']') { ++p; return v; }
        while (*p) {
            v.arr.push_back(parseVal());
            skip();
            if (*p == ',') { ++p; continue; }
            if (*p == ']') { ++p; break; }
            break;
        }
        return v;
    }
    std::string parseStr() {
        std::string s; ++p; // "
        while (*p && *p != '"') {
            if (*p == '\\') {
                ++p;
                switch (*p) {
                    case 'n': s += '\n'; break; case 't': s += '\t'; break;
                    case '"': s += '"'; break; case '\\': s += '\\'; break;
                    case '/': s += '/'; break; default: s += *p; break;
                }
                if (*p) ++p;
            } else s += *p++;
        }
        if (*p == '"') ++p;
        return s;
    }
    double parseNum() {
        char* end = nullptr;
        double d = strtod(p, &end);
        p = end ? end : p;
        return d;
    }
};
} // namespace

namespace forge {

void Tilemap::create(int w, int h, int tileW, int tileH) {
    w_ = w; h_ = h; tileW_ = tileW; tileH_ = tileH;
    layers_.clear();
    addLayer("main");
}

int Tilemap::addLayer(const std::string& name) {
    TileLayer l;
    l.name = name;
    l.width = w_; l.height = h_;
    l.tiles.assign((size_t)w_ * h_, 0);
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
    std::ifstream f(path);
    if (!f) {
        FORGE_LOG_ERROR("Tilemap file not found: %s", path.c_str());
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    Parser parser{text.c_str()};
    JVal root = parser.parse();
    if (root.type != JVal::Obj) {
        FORGE_LOG_ERROR("Tilemap '%s': invalid JSON", path.c_str());
        return false;
    }

    w_ = (int)root.find("width")->numOr(0);
    h_ = (int)root.find("height")->numOr(0);
    tileW_ = (int)root.find("tilewidth")->numOr(16);
    tileH_ = (int)root.find("tileheight")->numOr(16);
    layers_.clear();

    // tileset image (first tileset with image)
    const JVal* sets = root.find("tilesets");
    if (sets && sets->type == JVal::Arr && !sets->arr.empty()) {
        const JVal& ts = sets->arr[0];
        const JVal* img = ts.find("image");
        if (img && img->type == JVal::Str) {
            tilesetPath_ = img->str;
            const JVal* cols = ts.find("columns");
            const JVal* count = ts.find("tilecount");
            columns_ = cols ? (int)cols->numOr(0) : 0;
            tileCount_ = count ? (int)count->numOr(0) : 0;
        }
    }

    const JVal* layers = root.find("layers");
    if (!layers || layers->type != JVal::Arr) {
        FORGE_LOG_ERROR("Tilemap '%s': no layers", path.c_str());
        return false;
    }
    for (auto& lj : layers->arr) {
        const JVal* type = lj.find("type");
        if (!type || std::string(type->strOr("")) != "tilelayer") continue;
        TileLayer l;
        l.name = lj.find("name") ? lj.find("name")->strOr("layer") : "layer";
        l.width = (int)(lj.find("width") ? lj.find("width")->numOr(w_) : w_);
        l.height = (int)(lj.find("height") ? lj.find("height")->numOr(h_) : h_);
        l.visible = lj.find("visible") ? lj.find("visible")->boolOr(true) : true;
        l.opacity = (float)(lj.find("opacity") ? lj.find("opacity")->numOr(1.0) : 1.0);
        l.tiles.assign((size_t)l.width * l.height, 0);
        const JVal* data = lj.find("data");
        if (data && data->type == JVal::Arr) {
            for (size_t i = 0; i < data->arr.size() && i < l.tiles.size(); ++i)
                l.tiles[i] = (int)data->arr[i].numOr(0);
        }
        layers_.push_back(std::move(l));
    }
    FORGE_LOG_INFO("Tilemap '%s': %dx%d, %d layers", path.c_str(), w_, h_, (int)layers_.size());
    return true;
}

void Tilemap::setTileset(const Texture2D* tex, int columns, int tileCount) {
    tileset_ = tex;
    columns_ = columns;
    tileCount_ = tileCount;
}

void Tilemap::setTile(int layer, int x, int y, int id) {
    if (layer < 0 || layer >= (int)layers_.size()) return;
    if (x < 0 || y < 0 || x >= layers_[layer].width || y >= layers_[layer].height) return;
    layers_[layer].tiles[(size_t)y * layers_[layer].width + x] = id;
}

int Tilemap::tile(int layer, int x, int y) const {
    if (layer < 0 || layer >= (int)layers_.size()) return 0;
    const TileLayer& l = layers_[layer];
    if (x < 0 || y < 0 || x >= l.width || y >= l.height) return 0;
    return l.tiles[(size_t)y * l.width + x];
}

void Tilemap::setSolid(int tileId, bool solid) {
    if ((int)solid_.size() <= tileId) solid_.resize(tileId + 1, 0);
    solid_[tileId] = solid ? 1 : 0;
}

bool Tilemap::isSolid(int tileId) const {
    if (tileId < 0 || tileId >= (int)solid_.size()) return false;
    return solid_[tileId] != 0;
}

std::vector<Rect> Tilemap::solidsInRect(Rect r) const {
    std::vector<Rect> out;
    if (layers_.empty()) return out;
    int x0 = (int)(r.x / tileW_) - 1, x1 = (int)((r.x + r.w) / tileW_) + 1;
    int y0 = (int)(r.y / tileH_) - 1, y1 = (int)((r.y + r.h) / tileH_) + 1;
    for (auto& l : layers_) {
        if (!l.visible) continue;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                if (x < 0 || y < 0 || x >= l.width || y >= l.height) continue;
                int id = l.tiles[(size_t)y * l.width + x];
                if (id != 0 && isSolid(id))
                    out.push_back({(float)x * tileW_, (float)y * tileH_, (float)tileW_, (float)tileH_});
            }
    }
    return out;
}

bool Tilemap::collideRect(Rect r, Vec2* resolve) const {
    auto solids = solidsInRect(r);
    Vec2 total{0, 0};
    bool hit = false;
    Circle c{r.center(), 0}; // use rect-vs-rect via overlap math
    (void)c;
    for (auto& s : solids) {
        if (!r.overlaps(s)) continue;
        hit = true;
        // push out along smallest axis
        float dx1 = (s.x + s.w) - r.x;   // push +x
        float dx2 = r.x + r.w - s.x;     // push -x (magnitude)
        float dy1 = (s.y + s.h) - r.y;
        float dy2 = r.y + r.h - s.y;
        float mx = std::min(dx1, dx2), my = std::min(dy1, dy2);
        if (mx < my) total.x += (dx1 < dx2 ? mx : -mx);
        else total.y += (dy1 < dy2 ? my : -my);
    }
    if (resolve) *resolve = total;
    return hit;
}

Rect Tilemap::tileUv(int id) const {
    // id is 1-based; tileset row-major
    int idx = id - 1;
    int cx = columns_ > 0 ? idx % columns_ : 0;
    int cy = columns_ > 0 ? idx / columns_ : 0;
    if (!tileset_) return {0, 0, 1, 1};
    float tw = (float)tileset_->width(), th = (float)tileset_->height();
    float px = (float)cx * tileW_, py = (float)cy * tileH_;
    // Textures load flipped (flipY=true) so GL row 0 is image top.
    // Tiled rows are top-down: flip V back.
    return {px / tw, 1.0f - (py + tileH_) / th, tileW_ / tw, tileH_ / th};
}

void Tilemap::draw(SpriteBatch& batch, Vec2 origin, Rect view) const {
    if (!tileset_ || !tileset_->valid() || layers_.empty()) return;
    int x0 = (int)((view.x - origin.x) / tileW_) - 1;
    int x1 = (int)((view.x + view.w - origin.x) / tileW_) + 1;
    int y0 = (int)((view.y - origin.y) / tileH_) - 1;
    int y1 = (int)((view.y + view.h - origin.y) / tileH_) + 1;

    for (auto& l : layers_) {
        if (!l.visible) continue;
        Color tint{1, 1, 1, l.opacity};
        for (int y = std::max(y0, 0); y <= std::min(y1, l.height - 1); ++y)
            for (int x = std::max(x0, 0); x <= std::min(x1, l.width - 1); ++x) {
                int id = l.tiles[(size_t)y * l.width + x];
                if (id <= 0) continue;
                SpriteDraw d;
                d.tex = tileset_;
                d.pos = {origin.x + x * tileW_ + tileW_ * 0.5f,
                         origin.y + y * tileH_ + tileH_ * 0.5f};
                d.size = {(float)tileW_, (float)tileH_};
                d.src = tileUv(id);
                d.color = tint;
                batch.draw(d);
            }
    }
}

void Tilemap::drawAll(SpriteBatch& batch, Vec2 origin) const {
    draw(batch, origin, {origin.x, origin.y, (float)w_ * tileW_, (float)h_ * tileH_});
}

} // namespace forge
