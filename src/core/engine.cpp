#include "forge/core/engine.hpp"
#include "forge/graphics/gl_loader.hpp"
#include "forge/core/logger.hpp"
#include "forge/utils/profiler.hpp"
#include <SDL3/SDL.h>
#include <cstring>

namespace forge {

static void* glProc(const char* name) {
    // SDL returns a function pointer; bit-copy it into void* (no UB cast).
    SDL_FunctionPointer p = SDL_GL_GetProcAddress(name);
    void* v = nullptr;
    static_assert(sizeof(v) == sizeof(p), "pointer size mismatch");
    std::memcpy(&v, &p, sizeof(v));
    return v;
}

Engine::Engine(const EngineConfig& cfg) : cfg_(cfg), stepper_(cfg.fixedStep, cfg.maxSteps) {
    if (!window_.valid()) {
        FORGE_LOG_FATAL("Engine: window creation failed, aborting");
        return;
    }
    if (!initGL()) return;
    if (!batch_.init()) return;
    // white texture must exist before any draw
    Texture2D::white();
    if (cfg_.initAudio) {
        audio_.init(); // soft-fail: game runs muted
        assets_.setAudio(&audio_);
    }
    // open gamepads
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    for (int i = 0; i < n && i < 4; ++i) {
        SDL_Gamepad* gp = SDL_OpenGamepad(ids[i]);
        if (gp) {
            gamepads_[i] = gp;
            input_.connectGamepad(i);
            FORGE_LOG_INFO("Gamepad %d: %s", i, SDL_GetGamepadName(gp));
        }
    }
    SDL_free(ids);
    glReady_ = true;
    FORGE_LOG_INFO("Forge engine ready");
}

Engine::~Engine() {
    for (int i = 0; i < 4; ++i) {
        if (gamepads_[i]) SDL_CloseGamepad((SDL_Gamepad*)gamepads_[i]);
    }
    batch_.shutdown();
    audio_.shutdown();
    SDL_Quit();
}

bool Engine::initGL() {
    if (!gl::load(glProc)) {
        FORGE_LOG_FATAL("Engine: OpenGL 3.3 functions missing");
        return false;
    }
    IVec2 d = window_.drawableSize();
    gl::glViewport(0, 0, d.x, d.y);
    const char* ver = (const char*)gl::glGetString(GL_VERSION);
    FORGE_LOG_INFO("OpenGL: %s", ver ? ver : "?");
    return true;
}

IVec2 Engine::viewport() const {
    return window_.drawableSize();
}

Mat4 Engine::defaultProj() const {
    IVec2 vp = viewport();
    return Mat4::ortho(-vp.x * 0.5f, vp.x * 0.5f, -vp.y * 0.5f, vp.y * 0.5f);
}

static MouseButton sdlToMouse(uint8_t b) {
    switch (b) {
        case SDL_BUTTON_LEFT: return MouseButton::Left;
        case SDL_BUTTON_RIGHT: return MouseButton::Right;
        case SDL_BUTTON_MIDDLE: return MouseButton::Middle;
        case SDL_BUTTON_X1: return MouseButton::X1;
        default: return MouseButton::X2;
    }
}

bool Engine::pumpEvents() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_EVENT_QUIT:
                quitting_ = true;
                break;
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
                int w, h;
                SDL_GetWindowSize((SDL_Window*)window_.nativeHandle(), &w, &h);
                window_.onResize(w, h);
                IVec2 d = window_.drawableSize();
                gl::glViewport(0, 0, d.x, d.y);
                break;
            }
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                if (!ev.key.repeat)
                    input_.setKeyFromScancode(ev.key.scancode, ev.key.down);
                if (cfg_.quitOnEscape && ev.key.scancode == SDL_SCANCODE_ESCAPE && ev.key.down)
                    quitting_ = true;
                break;
            case SDL_EVENT_TEXT_INPUT:
                input_.pushText(ev.text.text);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                input_.setMouseButton(sdlToMouse(ev.button.button), ev.button.down);
                break;
            case SDL_EVENT_MOUSE_MOTION: {
                IVec2 win = window_.size();
                IVec2 draw = window_.drawableSize();
                float sx = win.x > 0 ? (float)draw.x / win.x : 1.0f;
                float sy = win.y > 0 ? (float)draw.y / win.y : 1.0f;
                input_.setMousePos({ev.motion.x * sx, ev.motion.y * sy});
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL:
                input_.addMouseWheel(ev.wheel.x, ev.wheel.y);
                break;
            case SDL_EVENT_GAMEPAD_ADDED: {
                for (int i = 0; i < 4; ++i) {
                    if (!gamepads_[i]) {
                        SDL_Gamepad* gp = SDL_OpenGamepad(ev.gdevice.which);
                        if (gp) {
                            gamepads_[i] = gp;
                            input_.connectGamepad(i);
                            FORGE_LOG_INFO("Gamepad %d connected", i);
                        }
                        break;
                    }
                }
                break;
            }
            case SDL_EVENT_GAMEPAD_REMOVED: {
                SDL_JoystickID gone = ev.gdevice.which;
                for (int i = 0; i < 4; ++i) {
                    if (gamepads_[i] && SDL_GetGamepadID((SDL_Gamepad*)gamepads_[i]) == gone) {
                        SDL_CloseGamepad((SDL_Gamepad*)gamepads_[i]);
                        gamepads_[i] = nullptr;
                        input_.disconnectGamepad(i);
                        break;
                    }
                }
                break;
            }
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP: {
                int slot = -1;
                for (int i = 0; i < 4; ++i) {
                    if (gamepads_[i] && SDL_GetGamepadID((SDL_Gamepad*)gamepads_[i]) == ev.gbutton.which) {
                        slot = i; break;
                    }
                }
                if (slot >= 0 && ev.gbutton.button < 14)
                    input_.setGamepadButton(slot, (GamepadButton)ev.gbutton.button, ev.gbutton.down);
                break;
            }
            case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
                int slot = -1;
                for (int i = 0; i < 4; ++i) {
                    if (gamepads_[i] && SDL_GetGamepadID((SDL_Gamepad*)gamepads_[i]) == ev.gaxis.which) {
                        slot = i; break;
                    }
                }
                if (slot >= 0) {
                    float v = ev.gaxis.value < 0
                        ? ev.gaxis.value / 32768.0f
                        : ev.gaxis.value / 32767.0f;
                    input_.setGamepadAxis(slot, ev.gaxis.axis, v);
                }
                break;
            }
            default: break;
        }
    }
    // refresh mouse pos in drawable pixels (motion events are in window coords)
    float mx, my;
    SDL_MouseButtonFlags st = SDL_GetMouseState(&mx, &my);
    (void)st;
    IVec2 win = window_.size();
    IVec2 draw = window_.drawableSize();
    float sx = win.x > 0 ? (float)draw.x / win.x : 1.0f;
    float sy = win.y > 0 ? (float)draw.y / win.y : 1.0f;
    input_.setMousePos({mx * sx, my * sy});
    return !quitting_;
}

bool Engine::beginFrame(const Mat4& viewProj) {
    Profiler::instance().beginFrame();
    input_.beginFrame();
    if (!pumpEvents()) return false;
    float dt = time_.tick();
    (void)dt;
    batch_.resetStats();
    IVec2 vp = viewport();
    gl::glViewport(0, 0, vp.x, vp.y);
    batch_.begin(viewProj);
    return true;
}

void Engine::endFrame() {
    batch_.end();
    window_.swapBuffers();
    Profiler::instance().endFrame();
}

void Engine::runFixed(std::function<void(float)> update, std::function<void()> render) {
    Camera2D cam; // unused default; render() owns its batch begin/end
    (void)cam;
    while (!quitting_) {
        Profiler::instance().beginFrame();
        input_.beginFrame();
        if (!pumpEvents()) break;
        float dt = time_.tick();
        {
            FORGE_ZONE("update");
            stepper_.update(dt, update);
        }
        {
            FORGE_ZONE("render");
            batch_.resetStats();
            render();
        }
        window_.swapBuffers();
        Profiler::instance().endFrame();
        assets_.update();
    }
}

void Engine::runVariable(std::function<void(float)> frame) {
    while (!quitting_) {
        Profiler::instance().beginFrame();
        input_.beginFrame();
        if (!pumpEvents()) break;
        float dt = time_.tick();
        batch_.resetStats();
        frame(dt);
        window_.swapBuffers();
        Profiler::instance().endFrame();
        assets_.update();
    }
}

} // namespace forge

// SDL3 requires SDL_main entry point handling
#include <SDL3/SDL_main.h>
