#include "forge/graphics/framebuffer.hpp"
#include "forge/graphics/gl_loader.hpp"

namespace forge {

Framebuffer::~Framebuffer() { destroy(); }

Framebuffer::Framebuffer(Framebuffer&& o) noexcept
    : fbo_(o.fbo_), tex_(o.tex_), w_(o.w_), h_(o.h_) {
    o.fbo_ = o.tex_ = 0; o.w_ = o.h_ = 0;
}
Framebuffer& Framebuffer::operator=(Framebuffer&& o) noexcept {
    if (this != &o) {
        destroy();
        fbo_ = o.fbo_; tex_ = o.tex_; w_ = o.w_; h_ = o.h_;
        o.fbo_ = o.tex_ = 0; o.w_ = o.h_ = 0;
    }
    return *this;
}

bool Framebuffer::create(int w, int h) {
    destroy();
    if (w <= 0 || h <= 0) return false;
    w_ = w; h_ = h;

    gl::glGenTextures(1, &tex_);
    gl::glBindTexture(GL_TEXTURE_2D, tex_);
    gl::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    gl::glGenFramebuffers(1, &fbo_);
    gl::glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    gl::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_, 0);
    bool ok = gl::glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    gl::glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!ok) { destroy(); return false; }
    return true;
}

void Framebuffer::destroy() {
    if (!gl::isLoaded()) { fbo_ = tex_ = 0; return; }
    if (fbo_) { gl::glDeleteFramebuffers(1, &fbo_); fbo_ = 0; }
    if (tex_) { gl::glDeleteTextures(1, &tex_); tex_ = 0; }
    w_ = h_ = 0;
}

void Framebuffer::bind() const {
    gl::glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    gl::glViewport(0, 0, w_, h_);
}

void Framebuffer::unbind(int vpW, int vpH) {
    gl::glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl::glViewport(0, 0, vpW, vpH);
}

} // namespace forge
