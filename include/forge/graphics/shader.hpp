#pragma once
// Shader: compile/link, uniform-location cache, built-in 2D sources.

#include "forge/math/math.hpp"
#include <string>
#include <unordered_map>
#include <cstdint>

namespace forge {

class Shader {
public:
    Shader() = default;
    Shader(const char* vertSrc, const char* fragSrc) { load(vertSrc, fragSrc); }
    ~Shader();

    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
    Shader(Shader&& o) noexcept;
    Shader& operator=(Shader&& o) noexcept;

    bool load(const char* vertSrc, const char* fragSrc);
    bool loadFiles(const std::string& vertPath, const std::string& fragPath);
    void destroy();
    bool valid() const { return id_ != 0; }
    void bind() const;

    void setInt(const char* name, int v);
    void setFloat(const char* name, float v);
    void setVec2(const char* name, Vec2 v);
    void setVec4(const char* name, float x, float y, float z, float w);
    void setMat4(const char* name, const Mat4& m);
    void setTextureSlots(const char* name, const int* slots, int count);

    uint32_t id() const { return id_; }

    // Built-in sprite shader (pos/uv/color/texIndex, 16 texture slots)
    static const char* kSpriteVert;
    static const char* kSpriteFrag;

private:
    int uniformLoc(const char* name);
    uint32_t id_ = 0;
    std::unordered_map<std::string, int> cache_;
};

} // namespace forge
