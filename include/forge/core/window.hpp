#pragma once
// Window: SDL3 + OpenGL 3.3 core context, VSync, resize handling.

#include "forge/math/math.hpp"
#include <string>
#include <cstdint>

// Opaque handles (SDL types stay in the .cpp to avoid header coupling)
namespace forge {

struct WindowConfig {
    int width = 1280;
    int height = 720;
    std::string title = "Forge Game";
    bool resizable = true;
    bool vsync = true;
    bool fullscreen = false;
    bool highDPI = true;
    int msaaSamples = 0; // 0, 4, 8
};

class Window {
public:
    explicit Window(const WindowConfig& cfg = {});
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&) noexcept;
    Window& operator=(Window&&) noexcept;

    bool valid() const { return window_ != nullptr; }
    void swapBuffers();
    void setTitle(const std::string& t);
    void setVSync(bool on);
    bool vsync() const { return vsync_; }

    IVec2 size() const;
    IVec2 drawableSize() const; // pixels (HIDPI aware)
    float aspect() const;
    float dpiScale() const;     // drawable / window

    void* nativeHandle() const { return window_; }
    void* glContext() const { return glContext_; }

    // Called by Engine on SDL_EVENT_WINDOW_RESIZED etc.
    void onResize(int w, int h);

private:
    void create(const WindowConfig& cfg);
    void destroy();

    void* window_ = nullptr;
    void* glContext_ = nullptr;
    WindowConfig cfg_;
    bool vsync_ = true;
    int w_ = 0, h_ = 0;
};

} // namespace forge
