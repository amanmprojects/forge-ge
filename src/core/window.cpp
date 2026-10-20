#include "forge/core/window.hpp"
#include "forge/core/logger.hpp"
#include "forge/core/platform.hpp"
#include <SDL3/SDL.h>

namespace forge {

Window::Window(const WindowConfig& cfg) { create(cfg); }

Window::~Window() { destroy(); }

Window::Window(Window&& o) noexcept
    : window_(o.window_), glContext_(o.glContext_), cfg_(std::move(o.cfg_)),
      vsync_(o.vsync_), w_(o.w_), h_(o.h_) {
    o.window_ = nullptr;
    o.glContext_ = nullptr;
}

Window& Window::operator=(Window&& o) noexcept {
    if (this != &o) {
        destroy();
        window_ = o.window_; glContext_ = o.glContext_;
        cfg_ = std::move(o.cfg_); vsync_ = o.vsync_; w_ = o.w_; h_ = o.h_;
        o.window_ = nullptr; o.glContext_ = nullptr;
    }
    return *this;
}

void Window::create(const WindowConfig& cfg) {
    cfg_ = cfg;
    w_ = cfg.width; h_ = cfg.height;

    if (!(SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO)) {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
            FORGE_LOG_ERROR("SDL_Init failed: %s", SDL_GetError());
            return;
        }
    }

#if FORGE_PLATFORM_WEB
    // WebGL2 is OpenGL ES 3.0; there is no 3.3 core profile in the browser.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    if (cfg.msaaSamples > 0) {
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, cfg.msaaSamples);
    } else {
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
    }

    SDL_WindowFlags flags = SDL_WINDOW_OPENGL;
    if (cfg.resizable) flags |= SDL_WINDOW_RESIZABLE;
    if (cfg.fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
    if (cfg.highDPI) flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;

    window_ = SDL_CreateWindow(cfg.title.c_str(), cfg.width, cfg.height, flags);
    if (!window_) {
        FORGE_LOG_ERROR("SDL_CreateWindow failed: %s", SDL_GetError());
        return;
    }
    glContext_ = SDL_GL_CreateContext((SDL_Window*)window_);
    if (!glContext_) {
        FORGE_LOG_ERROR("SDL_GL_CreateContext failed: %s", SDL_GetError());
        SDL_DestroyWindow((SDL_Window*)window_);
        window_ = nullptr;
        return;
    }
    SDL_GL_MakeCurrent((SDL_Window*)window_, (SDL_GLContext)glContext_);
    setVSync(cfg.vsync);

    int w, h;
    SDL_GetWindowSize((SDL_Window*)window_, &w, &h);
    w_ = w; h_ = h;
#if FORGE_PLATFORM_WEB
    FORGE_LOG_INFO("Window '%s' %dx%d created (WebGL2 / GL ES 3.0)", cfg.title.c_str(), w_, h_);
#else
    FORGE_LOG_INFO("Window '%s' %dx%d created (GL 3.3 core)", cfg.title.c_str(), w_, h_);
#endif
}

void Window::destroy() {
    if (glContext_) { SDL_GL_DestroyContext((SDL_GLContext)glContext_); glContext_ = nullptr; }
    if (window_) { SDL_DestroyWindow((SDL_Window*)window_); window_ = nullptr; }
}

void Window::swapBuffers() {
    if (window_) SDL_GL_SwapWindow((SDL_Window*)window_);
}

void Window::setTitle(const std::string& t) {
    cfg_.title = t;
    if (window_) SDL_SetWindowTitle((SDL_Window*)window_, t.c_str());
}

void Window::setVSync(bool on) {
    if (window_) {
        if (!SDL_GL_SetSwapInterval(on ? 1 : 0))
            SDL_GL_SetSwapInterval(0); // fallback to immediate
        // Cache what the driver actually gave us, not what we asked for.
        int actual = 0;
        if (SDL_GL_GetSwapInterval(&actual)) vsync_ = (actual != 0);
        else vsync_ = false;
    } else {
        vsync_ = on;
    }
}

IVec2 Window::size() const { return {w_, h_}; }

IVec2 Window::drawableSize() const {
    if (!window_) return {w_, h_};
    int w, h;
    SDL_GetWindowSizeInPixels((SDL_Window*)window_, &w, &h);
    return {w, h};
}

float Window::aspect() const {
    return h_ != 0 ? (float)w_ / (float)h_ : 1.0f;
}

float Window::dpiScale() const {
    IVec2 d = drawableSize();
    if (w_ == 0) return 1.0f;
    return (float)d.x / (float)w_;
}

void Window::onResize(int w, int h) {
    w_ = w > 0 ? w : 1; // minimized windows can report 0; keep ortho valid
    h_ = h > 0 ? h : 1;
}

} // namespace forge
