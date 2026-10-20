#include "forge/graphics/atlas.hpp"
#include <algorithm>
#include <cstring>

namespace forge {

AtlasPacker::AtlasPacker(int pageW, int pageH, int padding)
    : pageW_(pageW > 0 ? pageW : 1024), pageH_(pageH > 0 ? pageH : 1024),
      padding_(padding < 0 ? 0 : padding) {}

int AtlasPacker::add(int w, int h, const std::string& name) {
    if (w <= 0 || h <= 0) return -1;
    if (w + padding_ * 2 > pageW_ || h + padding_ * 2 > pageH_) return -1;
    Entry e;
    e.name = name;
    e.w = w; e.h = h;
    entries_.push_back(std::move(e));
    packed_ = false;
    return (int)entries_.size() - 1;
}

int AtlasPacker::add(const Rect& srcRect, const std::string& name) {
    int idx = add((int)srcRect.w, (int)srcRect.h, name);
    if (idx >= 0) entries_[(size_t)idx].src = srcRect;
    return idx;
}

bool AtlasPacker::pack() {
    pixels_.assign((size_t)pageW_ * pageH_ * 4, 0);
    // Skyline: for each x column, the current occupied height. Simpler and
    // better packed than a plain shelf for mixed sprite sizes.
    std::vector<int> skyline((size_t)pageW_, 0);
    bool allFit = true;

    // Tallest first: reduces fragmentation.
    std::vector<int> order(entries_.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = (int)i;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return entries_[(size_t)a].h > entries_[(size_t)b].h;
    });

    for (int idx : order) {
        Entry& e = entries_[(size_t)idx];
        e.placed = false;
        int needW = e.w + padding_ * 2;
        int bestY = pageH_ + 1, bestX = 0;
        // Scan candidate x positions at every skyline break point.
        for (int x = 0; x + needW <= pageW_; ++x) {
            int y = 0;
            for (int k = 0; k < needW; ++k) y = std::max(y, skyline[(size_t)(x + k)]);
            if (y + e.h + padding_ * 2 > pageH_) continue;
            if (y < bestY) { bestY = y; bestX = x; if (y == 0) break; }
        }
        if (bestY > pageH_) { allFit = false; continue; }
        int px = bestX + padding_;
        int py = bestY + padding_;
        e.out = {(float)px, (float)py, (float)e.w, (float)e.h};
        e.placed = true;
        int top = py + e.h + padding_;
        for (int k = 0; k < needW; ++k) skyline[(size_t)(bestX + k)] = top;
    }
    packed_ = true;
    return allFit;
}

const std::string& AtlasPacker::name(int index) const {
    static const std::string empty;
    if (index < 0 || index >= (int)entries_.size()) return empty;
    return entries_[(size_t)index].name;
}

Rect AtlasPacker::rect(int index) const {
    if (index < 0 || index >= (int)entries_.size()) return {0, 0, 0, 0};
    return entries_[(size_t)index].out;
}

int AtlasPacker::indexOf(const std::string& name) const {
    for (size_t i = 0; i < entries_.size(); ++i)
        if (entries_[i].name == name) return (int)i;
    return -1;
}

float AtlasPacker::occupancy() const {
    if (!packed_ || pageW_ <= 0 || pageH_ <= 0) return 0;
    double used = 0;
    for (auto& e : entries_)
        if (e.placed) used += (double)e.w * e.h;
    return (float)(used / ((double)pageW_ * pageH_));
}

bool AtlasPacker::blit(int index, const uint8_t* rgba, int srcW, int srcH, int srcChannels) {
    if (!packed_ || index < 0 || index >= (int)entries_.size()) return false;
    const Entry& e = entries_[(size_t)index];
    if (!e.placed || !rgba) return false;
    if (srcChannels < 1 || srcChannels > 4) return false;
    int w = std::min((int)e.w, srcW);
    int h = std::min((int)e.h, srcH);
    int dx = (int)e.out.x, dy = (int)e.out.y;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint8_t* s = rgba + ((size_t)y * srcW + x) * srcChannels;
            uint8_t* d = &pixels_[((size_t)(dy + y) * pageW_ + (dx + x)) * 4];
            if (srcChannels == 4) {
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
            } else if (srcChannels == 3) {
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
            } else if (srcChannels == 2) {
                d[0] = s[0]; d[1] = s[0]; d[2] = s[0]; d[3] = s[1];
            } else {
                d[0] = 255; d[1] = 255; d[2] = 255; d[3] = s[0];
            }
        }
    }
    return true;
}

} // namespace forge
