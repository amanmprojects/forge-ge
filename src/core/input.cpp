#include "forge/core/input.hpp"
#include <SDL3/SDL.h>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <string_view>
#include <utility>

namespace forge {

Input::Input() = default;
Input::~Input() = default;

void Input::beginFrame() {
    endFrame();
}

void Input::endFrame() {
    // Commit the current input window: fold current -> previous and clear the
    // one-shot state (latches, wheel, text). Engine::runFixed/runVariable call
    // this after each observed update step, so a down-edge latched during a
    // frame that ran no steps stays visible until some update sees it.
    std::memcpy(prevKeys_, keys_, sizeof(keys_));
    std::memcpy(prevMouse_, mouse_, sizeof(mouse_));
    std::memcpy(prevPadBtns_, padBtns_, sizeof(padBtns_));
    prevMousePos_ = mousePos_;
    wheelX_ = 0; wheelY_ = 0;
    std::memset(keyPressLatch_, 0, sizeof(keyPressLatch_));
    std::memset(keyReleaseLatch_, 0, sizeof(keyReleaseLatch_));
    std::memset(mousePressLatch_, 0, sizeof(mousePressLatch_));
    std::memset(mouseReleaseLatch_, 0, sizeof(mouseReleaseLatch_));
    std::memset(padPressLatch_, 0, sizeof(padPressLatch_));
    std::memset(padReleaseLatch_, 0, sizeof(padReleaseLatch_));
    textBuf_.clear();
    composition_.clear();
    compositionCursor_ = -1;
    touchPressLatch_ = false;
    touchReleaseLatch_ = false;
}

void Input::handleWindowEvent() {}

void Input::setKey(Key k, bool down) {
    int i = (int)k;
    if (i <= 0 || i >= kKeys) return;
    if (down != keys_[i]) {
        if (down) keyPressLatch_[i] = true;
        else keyReleaseLatch_[i] = true;
    }
    keys_[i] = down;
}

void Input::setMouseButton(MouseButton b, bool down) {
    int i = (int)b;
    if (i < 0 || i >= kMouse) return;
    if (down != mouse_[i]) {
        if (down) mousePressLatch_[i] = true;
        else mouseReleaseLatch_[i] = true;
    }
    mouse_[i] = down;
}

void Input::setGamepadButton(int pad, GamepadButton b, bool down) {
    if (pad < 0 || pad >= kPads) return;
    int i = (int)b;
    if (i < 0 || i >= kPadBtns) return;
    if (down != padBtns_[pad][i]) {
        if (down) padPressLatch_[pad][i] = true;
        else padReleaseLatch_[pad][i] = true;
    }
    padBtns_[pad][i] = down;
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
        std::memset(padPressLatch_[id], 0, sizeof(padPressLatch_[id]));
        std::memset(padReleaseLatch_[id], 0, sizeof(padReleaseLatch_[id]));
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
bool Input::pressed(Key k) const {
    int i = (int)k;
    if (i <= 0 || i >= kKeys) return false;
    return keyPressLatch_[i] || (keys_[i] && !prevKeys_[i]);
}
bool Input::released(Key k) const {
    int i = (int)k;
    if (i <= 0 || i >= kKeys) return false;
    return keyReleaseLatch_[i] || (!keys_[i] && prevKeys_[i]);
}

bool Input::mouseDown(MouseButton b) const {
    int i = (int)b;
    return i >= 0 && i < kMouse && mouse_[i];
}
bool Input::mousePressed(MouseButton b) const {
    int i = (int)b;
    if (i < 0 || i >= kMouse) return false;
    return mousePressLatch_[i] || (mouse_[i] && !prevMouse_[i]);
}
bool Input::mouseReleased(MouseButton b) const {
    int i = (int)b;
    if (i < 0 || i >= kMouse) return false;
    return mouseReleaseLatch_[i] || (!mouse_[i] && prevMouse_[i]);
}

bool Input::padDown(int pad, GamepadButton b) const {
    int i = (int)b;
    if (pad < 0 || pad >= kPads || i < 0 || i >= kPadBtns) return false;
    return padBtns_[pad][i];
}
bool Input::padPressed(int pad, GamepadButton b) const {
    int i = (int)b;
    if (pad < 0 || pad >= kPads || i < 0 || i >= kPadBtns) return false;
    return padPressLatch_[pad][i] || (padBtns_[pad][i] && !prevPadBtns_[pad][i]);
}
bool Input::padReleased(int pad, GamepadButton b) const {
    int i = (int)b;
    if (pad < 0 || pad >= kPads || i < 0 || i >= kPadBtns) return false;
    return padReleaseLatch_[pad][i] || (!padBtns_[pad][i] && prevPadBtns_[pad][i]);
}
float Input::padAxis(int pad, int axis) const {
    if (pad < 0 || pad >= kPads || axis < 0 || axis >= kAxes) return 0.0f;
    return axes_[pad][axis];
}
bool Input::anyPadDown(GamepadButton b) const {
    int i = (int)b;
    if (i < 0 || i >= kPadBtns) return false;
    for (int p = 0; p < kPads; ++p)
        if (padConnected_[p] && padBtns_[p][i]) return true;
    return false;
}
int Input::padCount() const {
    int n = 0;
    for (int p = 0; p < kPads; ++p) if (padConnected_[p]) ++n;
    return n;
}

void Input::pushText(const char* utf8) { if (utf8) textBuf_ += utf8; }
std::string Input::consumeText() { std::string t = std::move(textBuf_); textBuf_.clear(); return t; }

// ------------------------------------------------------------------- touch
const Input::TouchPoint* Input::touch(int64_t id) const {
    for (auto& t : touches_)
        if (t.id == id) return &t;
    return nullptr;
}

void Input::touchDown(int64_t id, Vec2 pos, float pressure) {
    for (auto& t : touches_) {
        if (t.id == id) { t.pos = pos; t.pressure = pressure; return; }
    }
    touches_.push_back(TouchPoint{id, pos, pos, pressure});
    touchPressLatch_ = true;
}

void Input::touchMove(int64_t id, Vec2 pos, float pressure) {
    for (auto& t : touches_) {
        if (t.id == id) { t.pos = pos; t.pressure = pressure; return; }
    }
    // A move without a down (missed event) is treated as a down.
    touchDown(id, pos, pressure);
}

void Input::touchUp(int64_t id) {
    for (size_t i = 0; i < touches_.size(); ++i) {
        if (touches_[i].id == id) {
            touches_.erase(touches_.begin() + (ptrdiff_t)i);
            touchReleaseLatch_ = true;
            return;
        }
    }
}

void Input::touchCancel(int64_t id) { touchUp(id); }

Vec2 Input::touchDrag(int64_t id) const {
    const TouchPoint* t = touch(id);
    return t ? t->pos - t->start : Vec2{0, 0};
}

// ---------------------------------------------------------- action remaps
namespace {

const char* keyName(Key k) {
    switch (k) {
        case Key::Space: return "space";
        case Key::Escape: return "escape";
        case Key::Enter: return "enter";
        case Key::Tab: return "tab";
        case Key::Backspace: return "backspace";
        case Key::Up: return "up";
        case Key::Down: return "down";
        case Key::Left: return "left";
        case Key::Right: return "right";
        case Key::LeftShift: return "lshift";
        case Key::RightShift: return "rshift";
        case Key::LeftCtrl: return "lctrl";
        case Key::RightCtrl: return "rctrl";
        case Key::LeftAlt: return "lalt";
        case Key::RightAlt: return "ralt";
        default: break;
    }
    int i = (int)k;
    if (i >= (int)Key::N0 && i <= (int)Key::N9) {
        static const char* digits[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
        return digits[i - (int)Key::N0];
    }
    if (i >= (int)Key::F1 && i <= (int)Key::F12) {
        static const char* fkeys[] = {"f1", "f2", "f3", "f4", "f5", "f6",
                                      "f7", "f8", "f9", "f10", "f11", "f12"};
        return fkeys[i - (int)Key::F1];
    }
    // The enum's letter block is not alphabetical, so map explicitly.
    static const std::pair<Key, const char*> letters[] = {
        {Key::A, "a"}, {Key::B, "b"}, {Key::C, "c"}, {Key::D, "d"}, {Key::E, "e"},
        {Key::F, "f"}, {Key::G, "g"}, {Key::H, "h"}, {Key::I, "i"}, {Key::J, "j"},
        {Key::K, "k"}, {Key::L, "l"}, {Key::M, "m"}, {Key::N, "n"}, {Key::O, "o"},
        {Key::P, "p"}, {Key::Q, "q"}, {Key::R, "r"}, {Key::S, "s"}, {Key::T, "t"},
        {Key::U, "u"}, {Key::V, "v"}, {Key::W, "w"}, {Key::X, "x"}, {Key::Y, "y"},
        {Key::Z, "z"}};
    for (const auto& [key, name] : letters)
        if (key == k) return name;
    return "unknown";
}

const char* mouseName(MouseButton b) {
    switch (b) {
        case MouseButton::Left: return "left";
        case MouseButton::Right: return "right";
        case MouseButton::Middle: return "middle";
        case MouseButton::X1: return "x1";
        case MouseButton::X2: return "x2";
    }
    return "left";
}

bool mouseFromName(const std::string& s, MouseButton& out) {
    if (s == "left") { out = MouseButton::Left; return true; }
    if (s == "right") { out = MouseButton::Right; return true; }
    if (s == "middle") { out = MouseButton::Middle; return true; }
    if (s == "x1") { out = MouseButton::X1; return true; }
    if (s == "x2") { out = MouseButton::X2; return true; }
    return false;
}

const char* padName(GamepadButton b) {
    switch (b) {
        case GamepadButton::A: return "a";
        case GamepadButton::B: return "b";
        case GamepadButton::X: return "x";
        case GamepadButton::Y: return "y";
        case GamepadButton::Back: return "back";
        case GamepadButton::Guide: return "guide";
        case GamepadButton::Start: return "start";
        case GamepadButton::LeftStick: return "ls";
        case GamepadButton::RightStick: return "rs";
        case GamepadButton::LeftShoulder: return "lb";
        case GamepadButton::RightShoulder: return "rb";
        case GamepadButton::DpadUp: return "up";
        case GamepadButton::DpadDown: return "down";
        case GamepadButton::DpadLeft: return "left";
        case GamepadButton::DpadRight: return "right";
        default: return "a";
    }
}

bool padFromName(const std::string& s, GamepadButton& out) {
    if (s == "a") { out = GamepadButton::A; return true; }
    if (s == "b") { out = GamepadButton::B; return true; }
    if (s == "x") { out = GamepadButton::X; return true; }
    if (s == "y") { out = GamepadButton::Y; return true; }
    if (s == "back") { out = GamepadButton::Back; return true; }
    if (s == "guide") { out = GamepadButton::Guide; return true; }
    if (s == "start") { out = GamepadButton::Start; return true; }
    if (s == "ls") { out = GamepadButton::LeftStick; return true; }
    if (s == "rs") { out = GamepadButton::RightStick; return true; }
    if (s == "lb") { out = GamepadButton::LeftShoulder; return true; }
    if (s == "rb") { out = GamepadButton::RightShoulder; return true; }
    if (s == "up") { out = GamepadButton::DpadUp; return true; }
    if (s == "down") { out = GamepadButton::DpadDown; return true; }
    if (s == "left") { out = GamepadButton::DpadLeft; return true; }
    if (s == "right") { out = GamepadButton::DpadRight; return true; }
    return false;
}

} // namespace

std::string Input::serializeBinds() const {
    std::string out;
    for (const auto& [action, b] : binds_) {
        out += action;
        out += '=';
        bool first = true;
        auto sep = [&] { if (!first) out += ','; first = false; };
        for (Key k : b.keys) { sep(); out += keyName(k); }
        for (auto m : b.mouse) { sep(); out += "mouse:"; out += mouseName(m); }
        for (auto p : b.pad) { sep(); out += "pad:"; out += padName(p); }
        out += '\n';
    }
    return out;
}

bool Input::deserializeBinds(const std::string& text, std::string* error) {
    std::unordered_map<std::string, Binds> parsed;
    size_t pos = 0;
    int lineNo = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? text.size() + 1 : end + 1;
        ++lineNo;
        // trim
        size_t a = line.find_first_not_of(" \t\r");
        if (a == std::string::npos) continue;
        size_t z = line.find_last_not_of(" \t\r");
        line = line.substr(a, z - a + 1);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            if (error) *error = "line " + std::to_string(lineNo) + ": expected action=bindings";
            return false;
        }
        std::string action = line.substr(0, eq);
        Binds b;
        std::string rest = line.substr(eq + 1);
        size_t sp = 0;
        while (sp <= rest.size()) {
            size_t comma = rest.find(',', sp);
            std::string item = rest.substr(sp, comma == std::string::npos ? std::string::npos : comma - sp);
            sp = comma == std::string::npos ? rest.size() + 1 : comma + 1;
            if (item.empty()) continue;
            if (item.rfind("mouse:", 0) == 0) {
                MouseButton m;
                if (!mouseFromName(item.substr(6), m)) {
                    if (error) *error = "line " + std::to_string(lineNo) + ": unknown mouse button '" + item + "'";
                    return false;
                }
                b.mouse.push_back(m);
            } else if (item.rfind("pad:", 0) == 0) {
                GamepadButton p;
                if (!padFromName(item.substr(4), p)) {
                    if (error) *error = "line " + std::to_string(lineNo) + ": unknown pad button '" + item + "'";
                    return false;
                }
                b.pad.push_back(p);
            } else {
                Key k = keyFromName(item);
                if (k == Key::Unknown) {
                    if (error) *error = "line " + std::to_string(lineNo) + ": unknown key '" + item + "'";
                    return false;
                }
                b.keys.push_back(k);
            }
        }
        parsed[action] = std::move(b);
    }
    binds_ = std::move(parsed);
    return true;
}

bool Input::saveBinds(const std::string& path) const {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::string text = serializeBinds();
    bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    std::fclose(f);
    return ok;
}

bool Input::loadBinds(const std::string& path, std::string* error) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);
    return deserializeBinds(text, error);
}

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
    for (int p = 0; p < kPads; ++p)
        if (padConnected_[p])
            for (auto b : it->second.pad)
                if (padReleased(p, b)) return true;
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
