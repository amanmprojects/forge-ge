#pragma once
// Input: keyboard / mouse / gamepad polling + action mapping.
// Polled once per frame by Engine; query with isDown / pressed / released.
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
enum class GamepadButton : uint8_t {
    A = 0, B, X, Y, Back, Start, Guide,
    LeftStick, RightStick, LeftShoulder, RightShoulder,
    DpadUp, DpadDown, DpadLeft, DpadRight
};

class Input {
public:
    Input();
    ~Input();

    // --- frame lifecycle (called by Engine) ---
    void beginFrame();   // copies current -> previous
    void endFrame() {}   // reserved
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
    float padAxis(int pad, int axis) const; // -1..1 (sticks), 0..1 (triggers)
    bool anyPadDown(GamepadButton b) const;
    int padCount() const;

    // Text input (for UI): fed by SDL_EVENT_TEXT_INPUT
    void pushText(const char* utf8);
    std::string consumeText();

    // --- action mapping ---
    void bindKey(const std::string& action, Key k);
    void bindMouse(const std::string& action, MouseButton b);
    void bindPad(const std::string& action, GamepadButton b);
    bool actionDown(const std::string& action) const;
    bool actionPressed(const std::string& action) const;
    bool actionReleased(const std::string& action) const;
    void clearBinds() { binds_.clear(); }

    // Axis-style movement helper: returns -1..1 vector from WASD/arrows/stick
    Vec2 moveAxis(int pad = -1) const;

    static Key keyFromName(std::string_view name);

private:
    static constexpr int kKeys = (int)Key::Count;
    static constexpr int kMouse = 5;
    static constexpr int kPads = 4;
    static constexpr int kPadBtns = 14;
    static constexpr int kAxes = 6;

    bool keys_[kKeys] = {};
    bool prevKeys_[kKeys] = {};
    bool mouse_[kMouse] = {};
    bool prevMouse_[kMouse] = {};
    bool padBtns_[kPads][kPadBtns] = {};
    bool prevPadBtns_[kPads][kPadBtns] = {};
    float axes_[kPads][kAxes] = {};
    bool padConnected_[kPads] = {};

    Vec2 mousePos_{}, prevMousePos_{};
    float wheelX_ = 0, wheelY_ = 0;
    std::string textBuf_;

    struct Binds {
        std::vector<Key> keys;
        std::vector<MouseButton> mouse;
        std::vector<GamepadButton> pad;
    };
    std::unordered_map<std::string, Binds> binds_;
};

} // namespace forge
