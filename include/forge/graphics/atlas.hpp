#pragma once
// AtlasPacker: shelf/skyline texture atlas packer.
//
// Feed it pixel rects (or image sizes) and it returns a packed layout inside a
// fixed page size. No image decoding: use it to build an atlas from your own
// pixel data (or from `Texture2D` sources you already loaded) and upload once.
//
//   forge::AtlasPacker packer(1024, 1024);
//   int a = packer.add(64, 32);
//   int b = packer.add(32, 32);
//   if (packer.pack()) {
//       Rect ra = packer.rect(a);
//       // blit pixels into packer.pixels() at ra, then Texture2D::loadMemory
//   }

#include "forge/math/math.hpp"
#include <vector>
#include <cstdint>
#include <string>

namespace forge {

class AtlasPacker {
public:
    // padding: transparent gutter between entries (prevents bleeding).
    explicit AtlasPacker(int pageW = 1024, int pageH = 1024, int padding = 2);

    // Returns a stable entry index, or -1 when the entry cannot fit the page.
    int add(int w, int h, const std::string& name = {});
    int add(const Rect& srcRect, const std::string& name = {});

    // Computes the layout. False when at least one entry does not fit.
    bool pack();

    size_t count() const { return entries_.size(); }
    const std::string& name(int index) const;
    Rect rect(int index) const;        // packed pixel rect (top-left origin)
    int indexOf(const std::string& name) const;

    int width() const { return pageW_; }
    int height() const { return pageH_; }
    float occupancy() const;           // 0..1, packed area / page area

    // RGBA8 page, transparent where nothing was packed. Filled by pack().
    const std::vector<uint8_t>& pixels() const { return pixels_; }
    std::vector<uint8_t>& pixels() { return pixels_; }

    // Copies an RGBA8 source image into its packed slot. Source rows are
    // top-down; the destination page is also top-down.
    bool blit(int index, const uint8_t* rgba, int srcW, int srcH, int srcChannels = 4);

private:
    struct Entry {
        std::string name;
        int w = 0, h = 0;
        Rect out{0, 0, 0, 0};
        bool placed = false;
        // Source rect for add(Rect) callers: the packer only needs the size,
        // the caller blits the source region itself.
        Rect src{0, 0, 0, 0};
    };

    int pageW_, pageH_, padding_;
    std::vector<Entry> entries_;
    std::vector<uint8_t> pixels_;
    bool packed_ = false;
};

} // namespace forge
