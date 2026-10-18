# 03 — Math

Header: `forge/math/math.hpp` — header-only, `constexpr` where possible,
`FORGE_INLINE` (always-inline) on hot functions. No dependencies.

## Constants and free functions

```cpp
inline constexpr float PI, TAU, DEG2RAD, RAD2DEG, EPSILON /* 1e-6f */;

float toRad(float deg);  float toDeg(float rad);
float clamp(float v, float lo, float hi);
float clamp01(float v);                       // 0..1
float lerp(float a, float b, float t);        // no clamping of t
float damp(float a, float b, float lambda, float dt); // exp smoothing: a→b
bool  approx(float a, float b, float eps = EPSILON);
```

`damp` is frame-rate independent (uses `1 - exp(-lambda*dt)`); prefer it over
`lerp(a, b, 0.1f)` for smoothing.

## Vec2

```cpp
struct Vec2 {
    float x = 0, y = 0;
    constexpr Vec2(); Vec2(float x, float y); explicit Vec2(float s); // (s,s)
    static constexpr Vec2 zero(), one(), right() /*{1,0}*/, up() /*{0,1}*/;

    // + - unary- *s /s *componentwise += -= *= /=  ==
    float lengthSq() const;  float length() const;
    Vec2  normalized() const;    // zero vector in → zero vector out (no NaN)
    float dot(Vec2) const;   float cross(Vec2) const;  // cross = x*oy - y*ox (scalar z)
    float angle() const;     // atan2(y,x), radians
    Vec2  rotated(float rad) const;            // CCW
    Vec2  lerpTo(Vec2, float t) const;
    float distTo(Vec2) const;
};
Vec2 operator*(float, Vec2);   // scalar on the left
```

Gotchas: `IVec2` does **not** convert to `Vec2` implicitly; do
`{(float)i.x, (float)i.y}` (or use `float(x)`), and `Vec2` has no
integer-truncating helpers.

## IVec2 / Vec4

```cpp
struct IVec2 { int x = 0, y = 0; Vec2 ctor; == + - };   // deliberately minimal
struct Vec4  { float x, y, z, w; Vec4 ctor; };
```

`IVec2` is used for viewport sizes, tile coordinates, grid math.

## Mat4 (column-major, OpenGL layout)

```cpp
struct Mat4 {
    float m[16];                       // identity by default
    static constexpr Mat4 identity();
    static Mat4 ortho(l, r, b, t, n = -1, f = 1);
    static Mat4 translate(Vec2);
    static Mat4 scale(Vec2);
    static Mat4 rotate(float rad);     // CCW
    Mat4 operator*(const Mat4&) const; // proj * view ordering
    Vec2 transformPoint(Vec2) const;   // ignores z/w (2D only)
    const float* data() const;
};
```

You rarely build matrices by hand: `Camera2D::viewProj` and
`Engine::defaultProj` produce them for the batch. If you do, note the
**column-major** layout and that `transformPoint` only uses x/y plus
translation.

## Rect

```cpp
struct Rect {
    float x = 0, y = 0, w = 0, h = 0;   // bottom-left origin, Y-up (x,y is the MIN corner)
    Rect(); Rect(x, y, w, h);
    static constexpr Rect fromCenter(Vec2 center, Vec2 size);
    Vec2 min() const; Vec2 max() const; Vec2 center() const; Vec2 size() const;
    bool contains(Vec2 p) const;              // INCLUSIVE edges
    bool overlaps(const Rect& o) const;       // STRICT: touching edges do not overlap
    Rect expanded(float amount) const;        // grows on all sides
};
```

`Rect` is bottom-left + Y-up like world space (not SDL-style top-left). This is
why tile and physics code can use it directly: `Body::aabb()` is
`Rect::fromCenter(pos, size)`.

## Circle

```cpp
struct Circle {
    Vec2 center; float radius = 0;
    bool contains(Vec2 p) const;
    bool overlaps(const Circle& o) const;   // <=, includes exact touch
};
```

## Color

RGBA floats `0..1` (alpha included).

```cpp
struct Color {
    float r = 1, g = 1, b = 1, a = 1;
    Color(); Color(r, g, b, a = 1);
    static constexpr Color white(), black(), clear(), red(), green(), blue(), yellow();
    static Color fromBytes(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);
    static Color fromHex(uint32_t hex);       // 0xRRGGBBAA (note: RGBA order)
    Color lerped(Color o, float t) const;
    Color withAlpha(float a) const;
    uint32_t pack() const;                    // R8G8B8A8 little-endian for GPU
};
```

`fromHex(0xE9C46AFF)` is the example convention (RGB first, alpha last).
`pack()` clamps and rounds — the batch stores packed colors.

## Random (PCG)

```cpp
class Random {
    explicit Random(uint64_t seed = 0x853c49e6748fea9bULL);
    void seed(uint64_t s);          // seed(0) is coerced to a fixed value, never degenerate
    uint32_t nextU32();
    float nextFloat();               // [0, 1)
    float range(float lo, float hi);
    int   rangeInt(int lo, int hi);  // INCLUSIVE, bias-free
    Vec2  inCircle(float radius = 1.0f);  // uniform disc
    bool  chance(float p);           // p in 0..1
};

Random& sharedRandom();   // thread_local, seeded from std::random_device → NOT deterministic
```

Use `Random` members with explicit seeds for reproducible gameplay (the
examples seed everything from `--seed`). Avoid `sharedRandom()` in code that
must replay identically.

## Easing (`forge::easing::*`)

All take/return `0..1` and are plain function pointers — directly assignable to
`Tween::ease`:

```cpp
linear, inQuad, outQuad, inOutQuad,
inCubic, outCubic, inOutCubic,
outElastic, outBounce, outBack
```

## Narrowphase collision helpers

```cpp
bool circleVsCircle(Circle a, Circle b);
bool rectVsRect(const Rect& a, const Rect& b);
bool circleVsRect(Circle c, const Rect& r);

// Minimum-translation push-out for the circle out of the rect.
// Works when the circle center is inside the rect (pushes along the
// smallest penetration axis). Returns {0,0} when not overlapping.
Vec2 resolveCircleRect(Circle c, const Rect& r);
```

Use these for bullets/coins/custom AI; use `PhysicsWorld` when you want
resolution, stacking and queries for many bodies.

## Value noise (deterministic, table-free)

```cpp
float hash01(int x, int y);              // 0..1 hash of an integer lattice point
float valueNoise2D(float x, float y);    // smooth 0..1 noise
float fbm2D(float x, float y, int octaves = 4); // fractal sum, normalized 0..1
```

Deterministic across runs (pure integer hash + lerp), which makes it safe for
procedural visuals replicated in headless captures. `Camera2D`'s shake uses
`valueNoise2D` so shake traces reproduce exactly.

## Cheat sheet: which math for which job

| Need | Use |
|---|---|
| Direction/distance | `Vec2::normalized`, `length`, `distTo` |
| Safe direction | `normalized()` (zero-length returns zero) |
| Smooth follow/damping | `damp(a, b, lambda, dt)` or `Camera2D::follow` |
| Random from seed | member `Random` |
| AABB overlap quick check | `Rect::overlaps` |
| Circle–rect resolution | `resolveCircleRect` |
| Shake/noise-driven motion | `valueNoise2D` / `fbm2D` |
| Color from a design value | `Color::fromHex(0xRRGGBBAA)` |
| Grid/tile coordinates | `IVec2` + integer division only where coordinates are non-negative; use `std::floor` otherwise |
