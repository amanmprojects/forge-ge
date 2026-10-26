#include "forge/core/engine.hpp"
#include "forge/graphics/gl_loader.hpp"
#include "forge/core/logger.hpp"
#include "forge/core/platform.hpp"
#include "forge/utils/profiler.hpp"
#include <SDL3/SDL.h>
#include <cstring>
#if FORGE_PLATFORM_WEB
#include <emscripten.h>
#endif

namespace forge {

static void* glProc(const char* name) {
    // SDL returns a function pointer; bit-copy it into void* (no UB cast).
    SDL_FunctionPointer p = SDL_GL_GetProcAddress(name);
    void* v = nullptr;
    static_assert(sizeof(v) == sizeof(p), "pointer size mismatch");
    std::memcpy(&v, &p, sizeof(v));
    return v;
}

Engine::Engine(const EngineConfig& cfg)
    : window_(cfg.window), cfg_(cfg), stepper_(cfg.fixedStep, cfg.maxSteps) {
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
    if (ids) {
        for (int i = 0; i < n && i < 4; ++i) {
            SDL_Gamepad* gp = SDL_OpenGamepad(ids[i]);
            if (gp) {
                gamepads_[i] = gp;
                input_.connectGamepad(i);
                FORGE_LOG_INFO("Gamepad %d: %s", i, SDL_GetGamepadName(gp));
            }
        }
        SDL_free(ids);
    }
    SDL_StartTextInput((SDL_Window*)window_.nativeHandle());
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

// Explicit map: SDL3 numeric order is ...BACK(4), GUIDE(5), START(6)...
// which does NOT match Forge's ...Back, Start, Guide... — never C-cast.
static bool sdlToPad(uint8_t sdl, GamepadButton& out) {
    using G = GamepadButton;
    switch (sdl) {
        case SDL_GAMEPAD_BUTTON_SOUTH: out = G::A; return true;
        case SDL_GAMEPAD_BUTTON_EAST: out = G::B; return true;
        case SDL_GAMEPAD_BUTTON_WEST: out = G::X; return true;
        case SDL_GAMEPAD_BUTTON_NORTH: out = G::Y; return true;
        case SDL_GAMEPAD_BUTTON_BACK: out = G::Back; return true;
        case SDL_GAMEPAD_BUTTON_GUIDE: out = G::Guide; return true;
        case SDL_GAMEPAD_BUTTON_START: out = G::Start; return true;
        case SDL_GAMEPAD_BUTTON_LEFT_STICK: out = G::LeftStick; return true;
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK: out = G::RightStick; return true;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: out = G::LeftShoulder; return true;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: out = G::RightShoulder; return true;
        case SDL_GAMEPAD_BUTTON_DPAD_UP: out = G::DpadUp; return true;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: out = G::DpadDown; return true;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: out = G::DpadLeft; return true;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: out = G::DpadRight; return true;
        default: return false; // MISC1, paddles: no Forge equivalent (yet)
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
                    quit();
                break;
            case SDL_EVENT_TEXT_INPUT:
                input_.pushText(ev.text.text);
                break;
            case SDL_EVENT_TEXT_EDITING:
                // IME composition preview. SDL gives the selection in UTF-8
                // byte offsets; cursor = start when there is no selection.
                // An empty composition means the IME session ended.
                input_.setComposition(ev.edit.text, ev.edit.start);
                break;
            case SDL_EVENT_FINGER_DOWN:
            case SDL_EVENT_FINGER_MOTION:
            case SDL_EVENT_FINGER_UP:
            case SDL_EVENT_FINGER_CANCELED: {
                // SDL finger coordinates are normalized (0..1) over the window;
                // convert to drawable pixels to match the mouse API.
                IVec2 draw = window_.drawableSize();
                Vec2 p{ev.tfinger.x * (float)draw.x, ev.tfinger.y * (float)draw.y};
                switch (ev.type) {
                    case SDL_EVENT_FINGER_DOWN:
                        input_.touchDown((int64_t)ev.tfinger.fingerID, p, ev.tfinger.pressure);
                        break;
                    case SDL_EVENT_FINGER_MOTION:
                        input_.touchMove((int64_t)ev.tfinger.fingerID, p, ev.tfinger.pressure);
                        break;
                    case SDL_EVENT_FINGER_UP:
                        input_.touchUp((int64_t)ev.tfinger.fingerID);
                        break;
                    default:
                        input_.touchCancel((int64_t)ev.tfinger.fingerID);
                        break;
                }
                break;
            }
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
                if (slot >= 0) {
                    GamepadButton fb;
                    if (sdlToPad(ev.gbutton.button, fb))
                        input_.setGamepadButton(slot, fb, ev.gbutton.down);
                }
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
    if (!pumpEvents()) {
        Profiler::instance().endFrame(); // keep profiler balanced on quit path
        return false;
    }
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

void Engine::clear(float r, float g, float b, float a) {
    IVec2 vp = viewport();
    gl::glViewport(0, 0, vp.x, vp.y);
    gl::glClearColor(r, g, b, a);
    gl::glClear(GL_COLOR_BUFFER_BIT);
}

void Engine::pace() {
    if (cfg_.maxFps <= 0) return;
    // Runs AFTER swap: sleep the remainder of the frame budget. Only useful
    // with vsync off (with vsync on the swap already blocks).
    uint64_t now = SDL_GetPerformanceCounter();
    uint64_t freq = SDL_GetPerformanceFrequency();
    double elapsed = (double)(now - lastSwap_) / (double)freq;
    double budget = 1.0 / (double)cfg_.maxFps;
    if (elapsed < budget) {
        uint64_t ns = (uint64_t)((budget - elapsed) * 1e9);
        if (ns > 0) SDL_DelayNS(ns);
    }
    lastSwap_ = SDL_GetPerformanceCounter();
}

void Engine::frameFixed() {
    Profiler::instance().beginFrame();
    if (!pumpEvents()) return; // quitting: no batch was begun (see runFixed)
    float dt = time_.tick();
    {
        FORGE_ZONE("update");
        stepper_.update(dt, [&](float step) {
            updateFn_(step);
            // Commit after each observed step: an edge latched during a
            // 0-step frame survives until an update sees it (exactly once).
            input_.endFrame();
        });
    }
    {
        FORGE_ZONE("render");
        batch_.resetStats();
        renderFn_();
    }
    window_.swapBuffers();
    pace();
    Profiler::instance().endFrame();
    assets_.update();
}

void Engine::frameVariable() {
    Profiler::instance().beginFrame();
    if (!pumpEvents()) return;
    float dt = time_.tick();
    batch_.resetStats();
    frameFn_(dt);
    input_.endFrame(); // frame() observed this window; commit it
    window_.swapBuffers();
    pace();
    Profiler::instance().endFrame();
    assets_.update();
}

void Engine::loopTrampoline(void* userdata) {
    Engine* e = (Engine*)userdata;
    if (e->quitting_) return; // loop was cancelled by quit()
    if (e->fixedLoop_) e->frameFixed();
    else e->frameVariable();
}

void Engine::runFixed(std::function<void(float)> update, std::function<void()> render) {
    updateFn_ = std::move(update);
    renderFn_ = std::move(render);
    fixedLoop_ = true;
#if FORGE_PLATFORM_WEB
    // The browser drives the frames. simulate_infinite_loop unwinds this
    // stack, so the engine and everything it points at must live outside it
    // (see docs/agents/01-getting-started.md, "Web build").
    emscripten_set_main_loop_arg(&Engine::loopTrampoline, this, 0, 1);
#else
    while (!quitting_) frameFixed();
#endif
}

void Engine::runVariable(std::function<void(float)> frame) {
    frameFn_ = std::move(frame);
    fixedLoop_ = false;
#if FORGE_PLATFORM_WEB
    emscripten_set_main_loop_arg(&Engine::loopTrampoline, this, 0, 1);
#else
    while (!quitting_) frameVariable();
#endif
}

} // namespace forge

// SDL3 entry point handling: including SDL_main.h without SDL_MAIN_HANDLED
// defines main() (Windows: WinMain) and renames the app's main to SDL_main on
// the platforms that need it. A library must not own the entry point - on
// Emscripten the app keeps a plain main() and emscripten calls it - so
// SDL_MAIN_HANDLED keeps the two from fighting over the symbol.
#define SDL_MAIN_HANDLED 1
#include <SDL3/SDL_main.h>
