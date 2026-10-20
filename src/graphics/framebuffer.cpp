#include "forge/graphics/framebuffer.hpp"
#include "forge/graphics/gl_loader.hpp"

namespace forge {

Framebuffer::~Framebuffer() { destroy(); }

Framebuffer::Framebuffer(Framebuffer&& o) noexcept
    : fbo_(o.fbo_), tex_(o.tex_), msFbo_(o.msFbo_), msColor_(o.msColor_),
      w_(o.w_), h_(o.h_), samples_(o.samples_) {
    o.fbo_ = o.tex_ = o.msFbo_ = o.msColor_ = 0;
    o.w_ = o.h_ = 0; o.samples_ = 0;
}
Framebuffer& Framebuffer::operator=(Framebuffer&& o) noexcept {
    if (this != &o) {
        destroy();
        fbo_ = o.fbo_; tex_ = o.tex_; msFbo_ = o.msFbo_; msColor_ = o.msColor_;
        w_ = o.w_; h_ = o.h_; samples_ = o.samples_;
        o.fbo_ = o.tex_ = o.msFbo_ = o.msColor_ = 0;
        o.w_ = o.h_ = 0; o.samples_ = 0;
    }
    return *this;
}

bool Framebuffer::create(int w, int h, int samples) {
    destroy();
    if (w <= 0 || h <= 0) return false;
    if (!gl::isLoaded()) return false;
    w_ = w; h_ = h;

    // Sampleable color texture (also the resolve destination).
    gl::glGenTextures(1, &tex_);
    gl::glBindTexture(GL_TEXTURE_2D, tex_);
    gl::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Resolve target.
    gl::glGenFramebuffers(1, &fbo_);
    gl::glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    gl::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_, 0);
    bool ok = gl::glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    // Multisampled render target, resolved into tex_ on demand.
    if (ok && samples > 1) {
        gl::glGenRenderbuffers(1, &msColor_);
        gl::glBindRenderbuffer(GL_RENDERBUFFER, msColor_);
        gl::glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, w, h);
        gl::glGenFramebuffers(1, &msFbo_);
        gl::glBindFramebuffer(GL_FRAMEBUFFER, msFbo_);
        gl::glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, msColor_);
        if (gl::glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            // Unsupported sample count: fall back to the plain target.
            gl::glBindFramebuffer(GL_FRAMEBUFFER, 0);
            gl::glDeleteFramebuffers(1, &msFbo_);
            gl::glDeleteRenderbuffers(1, &msColor_);
            msFbo_ = msColor_ = 0;
        } else {
            samples_ = samples;
        }
    }
    gl::glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl::glBindRenderbuffer(GL_RENDERBUFFER, 0);
    if (!ok) { destroy(); return false; }
    return true;
}

bool Framebuffer::resize(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (w == w_ && h == h_) return true;
    return create(w, h, samples_);
}

void Framebuffer::releaseAttachments() {
    if (!gl::isLoaded()) {
        fbo_ = tex_ = msFbo_ = msColor_ = 0;
        return;
    }
    if (msFbo_) { gl::glDeleteFramebuffers(1, &msFbo_); msFbo_ = 0; }
    if (msColor_) { gl::glDeleteRenderbuffers(1, &msColor_); msColor_ = 0; }
    if (fbo_) { gl::glDeleteFramebuffers(1, &fbo_); fbo_ = 0; }
    if (tex_) { gl::glDeleteTextures(1, &tex_); tex_ = 0; }
}

void Framebuffer::destroy() {
    releaseAttachments();
    w_ = h_ = 0;
    samples_ = 0;
}

void Framebuffer::bind() const {
    gl::glBindFramebuffer(GL_FRAMEBUFFER, msFbo_ ? msFbo_ : fbo_);
    gl::glViewport(0, 0, w_, h_);
}

void Framebuffer::unbind(int vpW, int vpH) {
    gl::glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl::glViewport(0, 0, vpW, vpH);
}

void Framebuffer::resolve() const {
    if (!msFbo_ || !fbo_) return;
    gl::glBindFramebuffer(GL_READ_FRAMEBUFFER, msFbo_);
    gl::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_);
    gl::glBlitFramebuffer(0, 0, w_, h_, 0, 0, w_, h_, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    gl::glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool Framebuffer::readPixels(std::vector<uint8_t>& out) const {
    if (!fbo_ || !gl::isLoaded()) return false;
    resolve(); // make the MSAA content visible in tex_
    out.resize((size_t)w_ * h_ * 4);
    gl::glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    gl::glPixelStorei(GL_PACK_ALIGNMENT, 1);
    gl::glReadPixels(0, 0, w_, h_, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
    gl::glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

} // namespace forge
