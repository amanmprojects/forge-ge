#pragma once
// Framebuffer: offscreen render target for post-processing / minimaps.

#include <cstdint>

namespace forge {

class Framebuffer {
public:
    Framebuffer() = default;
    ~Framebuffer();

    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;
    Framebuffer(Framebuffer&& o) noexcept;
    Framebuffer& operator=(Framebuffer&& o) noexcept;

    bool create(int w, int h);
    void destroy();
    bool valid() const { return fbo_ != 0; }

    void bind() const;   // render into
    static void unbind(int vpW, int vpH); // back to screen

    uint32_t texture() const { return tex_; }
    int width() const { return w_; }
    int height() const { return h_; }

private:
    uint32_t fbo_ = 0, tex_ = 0;
    int w_ = 0, h_ = 0;
};

} // namespace forge
