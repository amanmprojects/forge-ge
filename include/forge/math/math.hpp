#pragma once
// forge::math — header-only, constexpr-friendly 2D math. No dependencies.
// Layout is cache-friendly; hot functions are force-inlined.

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <random>

#ifndef FORGE_INLINE
#if defined(_MSC_VER)
#define FORGE_INLINE __forceinline
#else
#define FORGE_INLINE inline __attribute__((always_inline))
#endif
#endif

namespace forge {

// ---------------------------------------------------------------- constants
inline constexpr float PI      = 3.14159265358979323846f;
inline constexpr float TAU      = 6.28318530717958647692f;
inline constexpr float DEG2RAD = PI / 180.0f;
inline constexpr float RAD2DEG = 180.0f / PI;
inline constexpr float EPSILON = 1e-6f;

FORGE_INLINE float toRad(float deg) { return deg * DEG2RAD; }
FORGE_INLINE float toDeg(float rad) { return rad * RAD2DEG; }
FORGE_INLINE float clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
FORGE_INLINE float clamp01(float v) { return clamp(v, 0.0f, 1.0f); }
FORGE_INLINE float lerp(float a, float b, float t) { return a + (b - a) * t; }
FORGE_INLINE float damp(float a, float b, float lambda, float dt) {
    return lerp(a, b, 1.0f - std::exp(-lambda * dt));
}
FORGE_INLINE bool approx(float a, float b, float eps = EPSILON) {
    return std::fabs(a - b) <= eps;
}

// ---------------------------------------------------------------- Vec2
struct Vec2 {
    float x = 0.0f, y = 0.0f;
    constexpr Vec2() = default;
    constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}
    constexpr explicit Vec2(float s) : x(s), y(s) {}

    static constexpr Vec2 zero() { return {0, 0}; }
    static constexpr Vec2 one()  { return {1, 1}; }
    static constexpr Vec2 right(){ return {1, 0}; }
    static constexpr Vec2 up()   { return {0, 1}; }

    constexpr Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    constexpr Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    constexpr Vec2 operator-() const { return {-x, -y}; }
    constexpr Vec2 operator*(float s) const { return {x * s, y * s}; }
    constexpr Vec2 operator/(float s) const { return {x / s, y / s}; }
    constexpr Vec2 operator*(Vec2 o) const { return {x * o.x, y * o.y}; }
    constexpr Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }
    constexpr Vec2& operator-=(Vec2 o) { x -= o.x; y -= o.y; return *this; }
    constexpr Vec2& operator*=(float s) { x *= s; y *= s; return *this; }
    constexpr Vec2& operator/=(float s) { x /= s; y /= s; return *this; }
    constexpr bool operator==(Vec2 o) const { return x == o.x && y == o.y; }

    FORGE_INLINE float lengthSq() const { return x * x + y * y; }
    FORGE_INLINE float length() const { return std::sqrt(lengthSq()); }
    FORGE_INLINE Vec2 normalized() const {
        float l = length();
        return l > EPSILON ? Vec2{x / l, y / l} : Vec2{0, 0};
    }
    FORGE_INLINE float dot(Vec2 o) const { return x * o.x + y * o.y; }
    FORGE_INLINE float cross(Vec2 o) const { return x * o.y - y * o.x; }
    FORGE_INLINE float angle() const { return std::atan2(y, x); }
    FORGE_INLINE Vec2 rotated(float rad) const {
        float c = std::cos(rad), s = std::sin(rad);
        return {x * c - y * s, x * s + y * c};
    }
    FORGE_INLINE Vec2 lerpTo(Vec2 o, float t) const {
        return {lerp(x, o.x, t), lerp(y, o.y, t)};
    }
    FORGE_INLINE float distTo(Vec2 o) const { return (*this - o).length(); }
};
FORGE_INLINE Vec2 operator*(float s, Vec2 v) { return v * s; }

// ---------------------------------------------------------------- IVec2
struct IVec2 {
    int x = 0, y = 0;
    constexpr IVec2() = default;
    constexpr IVec2(int x_, int y_) : x(x_), y(y_) {}
    constexpr bool operator==(IVec2 o) const { return x == o.x && y == o.y; }
    constexpr IVec2 operator+(IVec2 o) const { return {x + o.x, y + o.y}; }
    constexpr IVec2 operator-(IVec2 o) const { return {x - o.x, y - o.y}; }
};

// ---------------------------------------------------------------- Vec4
struct Vec4 {
    float x = 0, y = 0, z = 0, w = 0;
    constexpr Vec4() = default;
    constexpr Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
};

// ---------------------------------------------------------------- Mat4 (column-major, OpenGL compatible)
struct Mat4 {
    float m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

    static constexpr Mat4 identity() { return Mat4{}; }
    static Mat4 ortho(float l, float r, float b, float t, float n = -1.0f, float f = 1.0f) {
        Mat4 o{};
        o.m[0] = 2.0f / (r - l);
        o.m[5] = 2.0f / (t - b);
        o.m[10] = -2.0f / (f - n);
        o.m[12] = -(r + l) / (r - l);
        o.m[13] = -(t + b) / (t - b);
        o.m[14] = -(f + n) / (f - n);
        return o;
    }
    static Mat4 translate(Vec2 t) {
        Mat4 o{};
        o.m[12] = t.x; o.m[13] = t.y;
        return o;
    }
    static Mat4 scale(Vec2 s) {
        Mat4 o{};
        o.m[0] = s.x; o.m[5] = s.y;
        return o;
    }
    static Mat4 rotate(float rad) {
        Mat4 o{};
        float c = std::cos(rad), s = std::sin(rad);
        o.m[0] = c;  o.m[1] = s;
        o.m[4] = -s; o.m[5] = c;
        return o;
    }
    Mat4 operator*(const Mat4& o) const {
        Mat4 r{};
        for (int c = 0; c < 4; ++c)
            for (int rr = 0; rr < 4; ++rr) {
                r.m[c * 4 + rr] = m[0 * 4 + rr] * o.m[c * 4 + 0]
                                + m[1 * 4 + rr] * o.m[c * 4 + 1]
                                + m[2 * 4 + rr] * o.m[c * 4 + 2]
                                + m[3 * 4 + rr] * o.m[c * 4 + 3];
            }
        return r;
    }
    Vec2 transformPoint(Vec2 p) const {
        return {
            m[0] * p.x + m[4] * p.y + m[12],
            m[1] * p.x + m[5] * p.y + m[13]
        };
    }
    const float* data() const { return m; }
};

// ---------------------------------------------------------------- Rect
struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    constexpr Rect() = default;
    constexpr Rect(float x_, float y_, float w_, float h_) : x(x_), y(y_), w(w_), h(h_) {}
    static constexpr Rect fromCenter(Vec2 c, Vec2 size) {
        return {c.x - size.x * 0.5f, c.y - size.y * 0.5f, size.x, size.y};
    }
    constexpr Vec2 min() const { return {x, y}; }
    constexpr Vec2 max() const { return {x + w, y + h}; }
    constexpr Vec2 center() const { return {x + w * 0.5f, y + h * 0.5f}; }
    constexpr Vec2 size() const { return {w, h}; }
    constexpr bool contains(Vec2 p) const {
        return p.x >= x && p.x <= x + w && p.y >= y && p.y <= y + h;
    }
    constexpr bool overlaps(const Rect& o) const {
        return x < o.x + o.w && x + w > o.x && y < o.y + o.h && y + h > o.y;
    }
    constexpr Rect expanded(float amount) const {
        return {x - amount, y - amount, w + amount * 2, h + amount * 2};
    }
};

// ---------------------------------------------------------------- Circle
struct Circle {
    Vec2 center{};
    float radius = 0;
    constexpr bool contains(Vec2 p) const {
        float dx = p.x - center.x, dy = p.y - center.y;
        return dx * dx + dy * dy <= radius * radius;
    }
    constexpr bool overlaps(const Circle& o) const {
        float dx = o.center.x - center.x, dy = o.center.y - center.y;
        float r = radius + o.radius;
        return dx * dx + dy * dy <= r * r;
    }
};

// ---------------------------------------------------------------- Color (RGBA, float 0..1)
struct Color {
    float r = 1, g = 1, b = 1, a = 1;
    constexpr Color() = default;
    constexpr Color(float r_, float g_, float b_, float a_ = 1.0f) : r(r_), g(g_), b(b_), a(a_) {}
    static constexpr Color white() { return {1,1,1,1}; }
    static constexpr Color black() { return {0,0,0,1}; }
    static constexpr Color clear() { return {0,0,0,0}; }
    static constexpr Color red()   { return {1,0,0,1}; }
    static constexpr Color green() { return {0,1,0,1}; }
    static constexpr Color blue()  { return {0,0,1,1}; }
    static constexpr Color yellow(){ return {1,1,0,1}; }
    static Color fromBytes(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
        return {r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
    }
    static Color fromHex(uint32_t hex) { // 0xRRGGBBAA
        return {
            ((hex >> 24) & 0xFF) / 255.0f,
            ((hex >> 16) & 0xFF) / 255.0f,
            ((hex >> 8) & 0xFF) / 255.0f,
            (hex & 0xFF) / 255.0f,
        };
    }
    Color lerped(Color o, float t) const {
        return {lerp(r,o.r,t), lerp(g,o.g,t), lerp(b,o.b,t), lerp(a,o.a,t)};
    }
    Color withAlpha(float alpha) const { return {r, g, b, alpha}; }
    // Pack as R8G8B8A8 for vertex buffers
    uint32_t pack() const {
        uint32_t R = (uint32_t)(clamp01(r) * 255.0f);
        uint32_t G = (uint32_t)(clamp01(g) * 255.0f);
        uint32_t B = (uint32_t)(clamp01(b) * 255.0f);
        uint32_t A = (uint32_t)(clamp01(a) * 255.0f);
        return R | (G << 8) | (B << 16) | (A << 24);
    }
};

// ---------------------------------------------------------------- Random (fast PCG, no global state)
class Random {
public:
    explicit Random(uint64_t seed = 0x853c49e6748fea9bULL) : state_(seed ? seed : 0x853c49e6748fea9bULL) {}
    void seed(uint64_t s) { state_ = s ? s : 1; }

    uint32_t nextU32() {
        uint64_t old = state_;
        state_ = old * 6364136223846793005ULL + 1442695040888963407ULL;
        uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
        uint32_t rot = (uint32_t)(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31));
    }
    float nextFloat() { return (nextU32() >> 8) * (1.0f / 16777216.0f); } // [0,1)
    float range(float lo, float hi) { return lo + nextFloat() * (hi - lo); }
    int rangeInt(int lo, int hi) { // inclusive
        return lo + (int)(nextU32() % (uint32_t)(hi - lo + 1));
    }
    Vec2 inCircle(float radius = 1.0f) {
        float a = nextFloat() * TAU;
        float r = std::sqrt(nextFloat()) * radius;
        return {std::cos(a) * r, std::sin(a) * r};
    }
    bool chance(float p) { return nextFloat() < p; }

private:
    uint64_t state_;
};

// Thread-local shared RNG for gameplay code
FORGE_INLINE Random& sharedRandom() {
    thread_local Random rng{std::random_device{}()};
    return rng;
}

// ---------------------------------------------------------------- Easing
namespace easing {
FORGE_INLINE float linear(float t) { return t; }
FORGE_INLINE float inQuad(float t) { return t * t; }
FORGE_INLINE float outQuad(float t) { return 1 - (1 - t) * (1 - t); }
FORGE_INLINE float inOutQuad(float t) { return t < 0.5f ? 2*t*t : 1 - std::pow(-2*t + 2, 2) / 2; }
FORGE_INLINE float inCubic(float t) { return t * t * t; }
FORGE_INLINE float outCubic(float t) { return 1 - std::pow(1 - t, 3); }
FORGE_INLINE float inOutCubic(float t) { return t < 0.5f ? 4*t*t*t : 1 - std::pow(-2*t + 2, 3) / 2; }
FORGE_INLINE float outElastic(float t) {
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    return std::pow(2, -10 * t) * std::sin((t * 10 - 0.75f) * (TAU / 3)) + 1;
}
FORGE_INLINE float outBounce(float t) {
    if (t < 1 / 2.75f) return 7.5625f * t * t;
    if (t < 2 / 2.75f) { t -= 1.5f / 2.75f; return 7.5625f * t * t + 0.75f; }
    if (t < 2.5f / 2.75f) { t -= 2.25f / 2.75f; return 7.5625f * t * t + 0.9375f; }
    t -= 2.625f / 2.75f; return 7.5625f * t * t + 0.984375f;
}
FORGE_INLINE float outBack(float t) {
    const float c = 1.70158f, c3 = c + 1;
    return 1 + c3 * std::pow(t - 1, 3) + c * std::pow(t - 1, 2);
}
} // namespace easing

// ---------------------------------------------------------------- Collision helpers (narrowphase)
FORGE_INLINE bool circleVsCircle(Circle a, Circle b) { return a.overlaps(b); }
FORGE_INLINE bool rectVsRect(const Rect& a, const Rect& b) { return a.overlaps(b); }
FORGE_INLINE bool circleVsRect(Circle c, const Rect& r) {
    float nx = clamp(c.center.x, r.x, r.x + r.w);
    float ny = clamp(c.center.y, r.y, r.y + r.h);
    float dx = c.center.x - nx, dy = c.center.y - ny;
    return dx * dx + dy * dy <= c.radius * c.radius;
}
// Minimum-translation-vector circle-vs-rect resolve. Returns push-out for circle.
FORGE_INLINE Vec2 resolveCircleRect(Circle c, const Rect& r) {
    float nx = clamp(c.center.x, r.x, r.x + r.w);
    float ny = clamp(c.center.y, r.y, r.y + r.h);
    Vec2 d{c.center.x - nx, c.center.y - ny};
    float len = d.length();
    if (len > EPSILON) {
        float push = c.radius - len;
        return push > 0 ? d * (push / len) : Vec2{0, 0};
    }
    // Center inside rect: push along smallest penetration axis
    float l = c.center.x - r.x, rr = r.x + r.w - c.center.x;
    float b = c.center.y - r.y, tt = r.y + r.h - c.center.y;
    float m = std::min(std::min(l, rr), std::min(b, tt));
    if (m == l) return {-l - c.radius, 0};
    if (m == rr) return {rr + c.radius, 0};
    if (m == b) return {0, -b - c.radius};
    return {0, tt + c.radius};
}

// ---------------------------------------------------------------- Value noise (deterministic, no tables)
FORGE_INLINE float hash01(int x, int y) {
    uint32_t h = (uint32_t)(x * 374761393 + y * 668265263);
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((h ^ (h >> 16)) >> 8) * (1.0f / 16777216.0f);
}
FORGE_INLINE float valueNoise2D(float x, float y) {
    int xi = (int)std::floor(x), yi = (int)std::floor(y);
    float xf = x - xi, yf = y - yi;
    float u = xf * xf * (3 - 2 * xf), v = yf * yf * (3 - 2 * yf);
    return lerp(lerp(hash01(xi, yi), hash01(xi + 1, yi), u),
                lerp(hash01(xi, yi + 1), hash01(xi + 1, yi + 1), u), v);
}
FORGE_INLINE float fbm2D(float x, float y, int octaves = 4) {
    float a = 0.5f, f = 1.0f, sum = 0.0f, norm = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        sum += a * valueNoise2D(x * f, y * f);
        norm += a; a *= 0.5f; f *= 2.0f;
    }
    return sum / norm;
}

} // namespace forge
