#pragma once
// Framebuffer: offscreen RGBA8 render target.
// Optional MSAA (renderbuffer + resolve), resize without losing the API
// contract, and a CPU readback helper for tests / screenshots.
//
// GL state: bind() sets the viewport to the target size. unbind() restores the
// default framebuffer and the viewport you pass. SpriteBatch tracks its own
// target, so an un-ended batch is still flushed into the right FBO.

#include "forge/math/math.hpp"
#include <cstdint>
#include <vector>

namespace forge {

class Framebuffer {
public:
    Framebuffer() = default;
    ~Framebuffer();

    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;
    Framebuffer(Framebuffer&& o) noexcept;
    Framebuffer& operator=(Framebuffer&& o) noexcept;

    // samples: 0 = no MSAA, 2/4/8 = multisampled color renderbuffer resolved
    // into the sampleable texture on resolve(). A driver that rejects the
    // sample count falls back to a non-MSAA target (returns true).
    bool create(int w, int h, int samples = 0);
    void destroy();
    bool valid() const { return fbo_ != 0; }

    // Recreate at a new size (keeps the sample count). No-op if unchanged.
    bool resize(int w, int h);

    void bind() const;   // render into (sets viewport)
    static void unbind(int vpW, int vpH); // back to screen
    // Resolve MSAA into the texture. No-op for non-MSAA targets.
    void resolve() const;

    // Reads RGBA8 pixels from the color attachment (bottom-left origin, the
    // GL convention). Returns false on an invalid target.
    bool readPixels(std::vector<uint8_t>& out) const;

    uint32_t texture() const { return tex_; }
    int width() const { return w_; }
    int height() const { return h_; }
    int samples() const { return samples_; }
    bool multisampled() const { return msFbo_ != 0; }

private:
    void releaseAttachments();

    uint32_t fbo_ = 0, tex_ = 0;
    uint32_t msFbo_ = 0, msColor_ = 0; // multisampled path (0 when samples == 0)
    int w_ = 0, h_ = 0;
    int samples_ = 0;
};

} // namespace forge
