#pragma once
// Input: keyboard / mouse / gamepad polling + action mapping.
// Engine feeds it from the SDL event pump; query with isDown / pressed /
// released. Edges are committed after each observed update step, so a press
// is never lost on a frame that ran no fixed steps (see Input::endFrame).
// Actions let gameplay bind "jump" -> {Space, GamepadA} instead of raw keys.

#include "forge/math/math.hpp"
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace forge {

enum class Key : int {
    Unknown = 0,
    Space, A, D, W, S, Up, Down, Left, Right,
    Escape, Enter, Tab, Backspace,
    LeftShift, RightShift, LeftCtrl, RightCtrl, LeftAlt, RightAlt,
    N0, N1, N2, N3, N4, N5, N6, N7, N8, N9,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Q, E, R, F, Z, X, C, V, B, N, M, H, J, K, L, U, I, O, P, T, G, Y,
    Minus, Equals, LBracket, RBracket, Semicolon, Quote, Comma, Period, Slash, Backslash,
    Count
};

enum class MouseButton : uint8_t { Left = 0, Right, Middle, X1, X2 };
// Forge-local gamepad layout (NOT the SDL numeric order — Engine maps
// SDL_GAMEPAD_BUTTON_* explicitly). A/B = south/east (Xbox layout).
enum class GamepadButton : uint8_t {
    A = 0, B, X, Y, Back, Start, Guide,
    LeftStick, RightStick, LeftShoulder, RightShoulder,
    DpadUp, DpadDown, DpadLeft, DpadRight,
    Count
};

class Input {
public:
    Input();
    ~Input();

    // --- frame lifecycle (called by Engine) ---
    // endFrame() commits the current input window: current -> previous and
    // clears the press/release latches, wheel and text buffer. The Engine
    // frame loops call it after each update step (or after frame(dt) in
    // runVariable), so an edge latched on a 0-step frame stays visible until
    // an update actually observes it, and is observed exactly once.
    // pressed()/released() also report taps that both went down AND up
    // between two commits (a key tapped inside one step would otherwise be
    // invisible to both queries).
    void beginFrame();   // starts an input window (= endFrame commit)
    void endFrame();     // commits the window, clears one-shot state
    void handleWindowEvent(); // pumps mouse position etc. if needed

    // Called by Engine's SDL event pump:
    void setKey(Key k, bool down);
    void setMouseButton(MouseButton b, bool down);
    void setMousePos(Vec2 p) { mousePos_ = p; }
    void addMouseWheel(float dx, float dy) { wheelX_ += dx; wheelY_ += dy; }
    void setGamepadButton(int pad, GamepadButton b, bool down);
    void setGamepadAxis(int pad, int axis, float v); // axis 0..5 (LX,LY,RX,RY,LT,RT)
    void connectGamepad(int id);
    void disconnectGamepad(int id);
    void setKeyFromScancode(uint32_t scancode, bool down);

    // --- queries ---
    bool isDown(Key k) const;
    bool pressed(Key k) const;   // went down this frame
    bool released(Key k) const;  // went up this frame

    bool mouseDown(MouseButton b) const;
    bool mousePressed(MouseButton b) const;
    bool mouseReleased(MouseButton b) const;
    Vec2 mousePos() const { return mousePos_; }
    Vec2 mouseDelta() const { return mousePos_ - prevMousePos_; }
    Vec2 wheel() const { return {wheelX_, wheelY_}; }

    bool padDown(int pad, GamepadButton b) const;
    bool padPressed(int pad, GamepadButton b) const;
    bool padReleased(int pad, GamepadButton b) const;
    float padAxis(int pad, int axis) const; // -1..1 (sticks), 0..1 (triggers)
    bool anyPadDown(GamepadButton b) const;
    int padCount() const;

    // Text input (for UI): fed by SDL_EVENT_TEXT_INPUT. `consumeText` clears
    // the buffer; `textBuffer` peeks. IME composition arrives through
    // `setComposition` and is reported separately from committed text.
    void pushText(const char* utf8);
    std::string consumeText();
    const std::string& textBuffer() const { return textBuf_; }
    // IME composition preview (SDL_EVENT_TEXT_EDITING). Empty = not composing.
    void setComposition(const char* utf8, int cursor = -1) {
        composition_ = utf8 ? utf8 : "";
        compositionCursor_ = cursor;
    }
    const std::string& composition() const { return composition_; }
    int compositionCursor() const { return compositionCursor_; }
    bool composing() const { return !composition_.empty(); }

    // ---- touch ----
    // SDL3 synthesizes mouse events from touch, so the mouse API keeps working;
    // this adds the raw multi-touch state (ids are SDL finger ids).
    struct TouchPoint {
        int64_t id = 0;
        Vec2 pos{};       // drawable pixels, top-left origin
        Vec2 start{};     // where the finger went down
        float pressure = 0.0f;
    };
    void touchDown(int64_t id, Vec2 pos, float pressure = 1.0f);
    void touchMove(int64_t id, Vec2 pos, float pressure = 1.0f);
    void touchUp(int64_t id);
    void touchCancel(int64_t id);
    // All active fingers (insertion order = down order).
    const std::vector<TouchPoint>& touches() const { return touches_; }
    const TouchPoint* touch(int64_t id) const;
    bool touchActive() const { return !touches_.empty(); }
    // True on the frame a finger first appeared (edge, committed per step).
    bool touchPressed() const { return touchPressLatch_; }
    bool touchReleased() const { return touchReleaseLatch_; }
    // Distance moved since the finger went down (drag gesture helper).
    Vec2 touchDrag(int64_t id) const;

    // --- action mapping ---
    void bindKey(const std::string& action, Key k);
    void bindMouse(const std::string& action, MouseButton b);
    void bindPad(const std::string& action, GamepadButton b);
    bool actionDown(const std::string& action) const;
    bool actionPressed(const std::string& action) const;
    bool actionReleased(const std::string& action) const;
    void clearBinds() { binds_.clear(); }
    // Serialized remaps: "action=KeyName,Mouse:left,Pad:a" lines, one action
    // per line. saveBinds/loadBinds make player remaps persistent.
    std::string serializeBinds() const;
    bool deserializeBinds(const std::string& text, std::string* error = nullptr);
    bool saveBinds(const std::string& path) const;
    bool loadBinds(const std::string& path, std::string* error = nullptr);

    // Stick response curve: 0 = linear, >0 adds a deadzone and a power curve
    // (1 = quadratic-ish). Applied by moveAxis().
    void setStickDeadzone(float dz) { stickDeadzone_ = clamp01(dz); }
    void setStickCurve(float power) { stickCurve_ = power < 0.0f ? 0.0f : power; }
    float stickDeadzone() const { return stickDeadzone_; }
    float stickCurve() const { return stickCurve_; }

    // Axis-style movement helper: returns -1..1 vector from WASD/arrows/stick
    Vec2 moveAxis(int pad = -1) const;

    static Key keyFromName(std::string_view name);

private:
    static constexpr int kKeys = (int)Key::Count;
    static constexpr int kMouse = 5;
    static constexpr int kPads = 4;
    static constexpr int kPadBtns = (int)GamepadButton::Count;
    static constexpr int kAxes = 6;

    bool keys_[kKeys] = {};
    bool prevKeys_[kKeys] = {};
    bool mouse_[kMouse] = {};
    bool prevMouse_[kMouse] = {};
    bool padBtns_[kPads][kPadBtns] = {};
    bool prevPadBtns_[kPads][kPadBtns] = {};
    float axes_[kPads][kAxes] = {};
    bool padConnected_[kPads] = {};

    // Set on a real down/up transition; cleared by endFrame(). See the
    // endFrame comment: keeps sub-step taps visible to pressed()/released().
    bool keyPressLatch_[kKeys] = {}, keyReleaseLatch_[kKeys] = {};
    bool mousePressLatch_[kMouse] = {}, mouseReleaseLatch_[kMouse] = {};
    bool padPressLatch_[kPads][kPadBtns] = {}, padReleaseLatch_[kPads][kPadBtns] = {};

    Vec2 mousePos_{}, prevMousePos_{};
    float wheelX_ = 0, wheelY_ = 0;
    std::string textBuf_;
    std::string composition_;
    int compositionCursor_ = -1;
    std::vector<TouchPoint> touches_;
    bool touchPressLatch_ = false, touchReleaseLatch_ = false;
    float stickDeadzone_ = 0.18f;
    float stickCurve_ = 1.0f;

    struct Binds {
        std::vector<Key> keys;
        std::vector<MouseButton> mouse;
        std::vector<GamepadButton> pad;
    };
    std::unordered_map<std::string, Binds> binds_;
};

} // namespace forge
