#include "forge/core/input.hpp"
#include <SDL3/SDL.h>
#include <cstring>

namespace forge {

Input::Input() = default;
Input::~Input() = default;

void Input::beginFrame() {
    std::memcpy(prevKeys_, keys_, sizeof(keys_));
    std::memcpy(prevMouse_, mouse_, sizeof(mouse_));
    std::memcpy(prevPadBtns_, padBtns_, sizeof(padBtns_));
    prevMousePos_ = mousePos_;
    wheelX_ = 0; wheelY_ = 0;
    textBuf_.clear();
}

void Input::handleWindowEvent() {}

void Input::setKey(Key k, bool down) {
    int i = (int)k;
    if (i > 0 && i < kKeys) keys_[i] = down;
}

void Input::setMouseButton(MouseButton b, bool down) {
    int i = (int)b;
    if (i >= 0 && i < kMouse) mouse_[i] = down;
}

void Input::setGamepadButton(int pad, GamepadButton b, bool down) {
    if (pad < 0 || pad >= kPads) return;
    padBtns_[pad][(int)b] = down;
}

void Input::setGamepadAxis(int pad, int axis, float v) {
    if (pad < 0 || pad >= kPads || axis < 0 || axis >= kAxes) return;
    // deadzone for sticks
    if (axis < 4 && std::fabs(v) < 0.12f) v = 0.0f;
    axes_[pad][axis] = v;
}

void Input::connectGamepad(int id) {
    if (id >= 0 && id < kPads) padConnected_[id] = true;
}
void Input::disconnectGamepad(int id) {
    if (id >= 0 && id < kPads) {
        padConnected_[id] = false;
        std::memset(padBtns_[id], 0, sizeof(padBtns_[id]));
        std::memset(axes_[id], 0, sizeof(axes_[id]));
    }
}

static Key scancodeToKey(SDL_Scancode s) {
    using K = Key;
    switch (s) {
        case SDL_SCANCODE_SPACE: return K::Space;
        case SDL_SCANCODE_A: return K::A; case SDL_SCANCODE_D: return K::D;
        case SDL_SCANCODE_W: return K::W; case SDL_SCANCODE_S: return K::S;
        case SDL_SCANCODE_UP: return K::Up; case SDL_SCANCODE_DOWN: return K::Down;
        case SDL_SCANCODE_LEFT: return K::Left; case SDL_SCANCODE_RIGHT: return K::Right;
        case SDL_SCANCODE_ESCAPE: return K::Escape; case SDL_SCANCODE_RETURN: return K::Enter;
        case SDL_SCANCODE_TAB: return K::Tab; case SDL_SCANCODE_BACKSPACE: return K::Backspace;
        case SDL_SCANCODE_LSHIFT: return K::LeftShift; case SDL_SCANCODE_RSHIFT: return K::RightShift;
        case SDL_SCANCODE_LCTRL: return K::LeftCtrl; case SDL_SCANCODE_RCTRL: return K::RightCtrl;
        case SDL_SCANCODE_LALT: return K::LeftAlt; case SDL_SCANCODE_RALT: return K::RightAlt;
        case SDL_SCANCODE_0: return K::N0; case SDL_SCANCODE_1: return K::N1;
        case SDL_SCANCODE_2: return K::N2; case SDL_SCANCODE_3: return K::N3;
        case SDL_SCANCODE_4: return K::N4; case SDL_SCANCODE_5: return K::N5;
        case SDL_SCANCODE_6: return K::N6; case SDL_SCANCODE_7: return K::N7;
        case SDL_SCANCODE_8: return K::N8; case SDL_SCANCODE_9: return K::N9;
        case SDL_SCANCODE_F1: return K::F1; case SDL_SCANCODE_F2: return K::F2;
        case SDL_SCANCODE_F3: return K::F3; case SDL_SCANCODE_F4: return K::F4;
        case SDL_SCANCODE_F5: return K::F5; case SDL_SCANCODE_F6: return K::F6;
        case SDL_SCANCODE_F7: return K::F7; case SDL_SCANCODE_F8: return K::F8;
        case SDL_SCANCODE_F9: return K::F9; case SDL_SCANCODE_F10: return K::F10;
        case SDL_SCANCODE_F11: return K::F11; case SDL_SCANCODE_F12: return K::F12;
        case SDL_SCANCODE_Q: return K::Q; case SDL_SCANCODE_E: return K::E;
        case SDL_SCANCODE_R: return K::R; case SDL_SCANCODE_F: return K::F;
        case SDL_SCANCODE_Z: return K::Z; case SDL_SCANCODE_X: return K::X;
        case SDL_SCANCODE_C: return K::C; case SDL_SCANCODE_V: return K::V;
        case SDL_SCANCODE_B: return K::B; case SDL_SCANCODE_N: return K::N;
        case SDL_SCANCODE_M: return K::M; case SDL_SCANCODE_H: return K::H;
        case SDL_SCANCODE_J: return K::J; case SDL_SCANCODE_K: return K::K;
        case SDL_SCANCODE_L: return K::L; case SDL_SCANCODE_U: return K::U;
        case SDL_SCANCODE_I: return K::I; case SDL_SCANCODE_O: return K::O;
        case SDL_SCANCODE_P: return K::P; case SDL_SCANCODE_T: return K::T;
        case SDL_SCANCODE_G: return K::G; case SDL_SCANCODE_Y: return K::Y;
        case SDL_SCANCODE_MINUS: return K::Minus; case SDL_SCANCODE_EQUALS: return K::Equals;
        case SDL_SCANCODE_LEFTBRACKET: return K::LBracket; case SDL_SCANCODE_RIGHTBRACKET: return K::RBracket;
        case SDL_SCANCODE_SEMICOLON: return K::Semicolon; case SDL_SCANCODE_APOSTROPHE: return K::Quote;
        case SDL_SCANCODE_COMMA: return K::Comma; case SDL_SCANCODE_PERIOD: return K::Period;
        case SDL_SCANCODE_SLASH: return K::Slash; case SDL_SCANCODE_BACKSLASH: return K::Backslash;
        default: return K::Unknown;
    }
}

void Input::setKeyFromScancode(uint32_t scancode, bool down) {
    setKey(scancodeToKey((SDL_Scancode)scancode), down);
}

bool Input::isDown(Key k) const { int i = (int)k; return i > 0 && i < kKeys && keys_[i]; }
bool Input::pressed(Key k) const { int i = (int)k; return i > 0 && i < kKeys && keys_[i] && !prevKeys_[i]; }
bool Input::released(Key k) const { int i = (int)k; return i > 0 && i < kKeys && !keys_[i] && prevKeys_[i]; }

bool Input::mouseDown(MouseButton b) const { return mouse_[(int)b]; }
bool Input::mousePressed(MouseButton b) const { return mouse_[(int)b] && !prevMouse_[(int)b]; }
bool Input::mouseReleased(MouseButton b) const { return !mouse_[(int)b] && prevMouse_[(int)b]; }

bool Input::padDown(int pad, GamepadButton b) const {
    if (pad < 0 || pad >= kPads) return false;
    return padBtns_[pad][(int)b];
}
bool Input::padPressed(int pad, GamepadButton b) const {
    if (pad < 0 || pad >= kPads) return false;
    return padBtns_[pad][(int)b] && !prevPadBtns_[pad][(int)b];
}
float Input::padAxis(int pad, int axis) const {
    if (pad < 0 || pad >= kPads || axis < 0 || axis >= kAxes) return 0.0f;
    return axes_[pad][axis];
}
bool Input::anyPadDown(GamepadButton b) const {
    for (int p = 0; p < kPads; ++p)
        if (padConnected_[p] && padBtns_[p][(int)b]) return true;
    return false;
}
int Input::padCount() const {
    int n = 0;
    for (int p = 0; p < kPads; ++p) if (padConnected_[p]) ++n;
    return n;
}

void Input::pushText(const char* utf8) { if (utf8) textBuf_ += utf8; }
std::string Input::consumeText() { std::string t = std::move(textBuf_); textBuf_.clear(); return t; }

void Input::bindKey(const std::string& action, Key k) { binds_[action].keys.push_back(k); }
void Input::bindMouse(const std::string& action, MouseButton b) { binds_[action].mouse.push_back(b); }
void Input::bindPad(const std::string& action, GamepadButton b) { binds_[action].pad.push_back(b); }

bool Input::actionDown(const std::string& action) const {
    auto it = binds_.find(action);
    if (it == binds_.end()) return false;
    for (Key k : it->second.keys) if (isDown(k)) return true;
    for (auto b : it->second.mouse) if (mouseDown(b)) return true;
    for (auto b : it->second.pad) if (anyPadDown(b)) return true;
    return false;
}
bool Input::actionPressed(const std::string& action) const {
    auto it = binds_.find(action);
    if (it == binds_.end()) return false;
    for (Key k : it->second.keys) if (pressed(k)) return true;
    for (auto b : it->second.mouse) if (mousePressed(b)) return true;
    for (int p = 0; p < kPads; ++p)
        for (auto b : it->second.pad)
            if (padConnected_[p] && padPressed(p, b)) return true;
    return false;
}
bool Input::actionReleased(const std::string& action) const {
    auto it = binds_.find(action);
    if (it == binds_.end()) return false;
    for (Key k : it->second.keys) if (released(k)) return true;
    for (auto b : it->second.mouse) if (mouseReleased(b)) return true;
    return false;
}

Vec2 Input::moveAxis(int pad) const {
    Vec2 v{0, 0};
    if (isDown(Key::A) || isDown(Key::Left)) v.x -= 1;
    if (isDown(Key::D) || isDown(Key::Right)) v.x += 1;
    if (isDown(Key::W) || isDown(Key::Up)) v.y += 1;
    if (isDown(Key::S) || isDown(Key::Down)) v.y -= 1;
    auto applyPad = [&](int p) {
        if (p < 0 || p >= kPads || !padConnected_[p]) return;
        v.x += axes_[p][0];
        v.y -= axes_[p][1]; // SDL Y down for sticks; world Y up
        if (padDown(p, GamepadButton::DpadLeft)) v.x -= 1;
        if (padDown(p, GamepadButton::DpadRight)) v.x += 1;
        if (padDown(p, GamepadButton::DpadUp)) v.y += 1;
        if (padDown(p, GamepadButton::DpadDown)) v.y -= 1;
    };
    if (pad >= 0) applyPad(pad);
    else for (int p = 0; p < kPads; ++p) applyPad(p);
    float len = v.length();
    if (len > 1.0f) v /= len;
    return v;
}

Key Input::keyFromName(std::string_view name) {
    if (name == "space") return Key::Space;
    if (name == "escape") return Key::Escape;
    if (name == "enter") return Key::Enter;
    if (name == "tab") return Key::Tab;
    if (name == "up") return Key::Up;
    if (name == "down") return Key::Down;
    if (name == "left") return Key::Left;
    if (name == "right") return Key::Right;
    if (name.size() == 1) {
        char c = (char)std::toupper((unsigned char)name[0]);
        if (c >= 'A' && c <= 'Z') {
            // Q..Y are contiguous in the enum after F-keys; map explicitly.
            static const Key letters[] = {Key::A, Key::B, Key::C, Key::D, Key::E, Key::F,
                Key::G, Key::H, Key::I, Key::J, Key::K, Key::L, Key::M, Key::N,
                Key::O, Key::P, Key::Q, Key::R, Key::S, Key::T, Key::U,
                Key::V, Key::W, Key::X, Key::Y, Key::Z};
            return letters[c - 'A'];
        }
        if (c >= '0' && c <= '9') return (Key)((int)Key::N0 + (c - '0'));
    }
    return Key::Unknown;
}

} // namespace forge
