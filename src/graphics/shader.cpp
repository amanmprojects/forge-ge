#include "forge/graphics/shader.hpp"
#include "forge/graphics/gl_loader.hpp"
#include "forge/core/logger.hpp"
#include "forge/core/platform.hpp"
#include <fstream>
#include <sstream>

namespace forge {

// Desktop GLSL 3.30 core vs. WebGL2 (GLSL ES 3.00). The bodies below are
// written in the subset both accept, so only the version directive and the
// ES-mandated float/int precision differ.
#if FORGE_PLATFORM_WEB
#define FORGE_GLSL_HEADER \
    "#version 300 es\nprecision highp float;\nprecision highp int;\n"
#else
#define FORGE_GLSL_HEADER "#version 330 core\n"
#endif

const char* Shader::kSpriteVert = FORGE_GLSL_HEADER R"GLSL(
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
layout(location = 3) in float aTexIndex;
out vec2 vUV;
out vec4 vColor;
out float vTexIndex;
uniform mat4 uProj;
void main() {
    vUV = aUV;
    vColor = aColor;
    vTexIndex = aTexIndex;
    gl_Position = uProj * vec4(aPos, 0.0, 1.0);
}
)GLSL";

const char* Shader::kSpriteFrag = FORGE_GLSL_HEADER R"GLSL(
in vec2 vUV;
in vec4 vColor;
in float vTexIndex;
out vec4 FragColor;
uniform sampler2D uTextures[16];
void main() {
    // NOTE: dynamic sampler indexing is forbidden in GLSL 3.30 (and in ES
    // 3.00), so unroll.
    int idx = int(vTexIndex + 0.5);
    vec4 tex;
    if (idx == 0) tex = texture(uTextures[0], vUV);
    else if (idx == 1) tex = texture(uTextures[1], vUV);
    else if (idx == 2) tex = texture(uTextures[2], vUV);
    else if (idx == 3) tex = texture(uTextures[3], vUV);
    else if (idx == 4) tex = texture(uTextures[4], vUV);
    else if (idx == 5) tex = texture(uTextures[5], vUV);
    else if (idx == 6) tex = texture(uTextures[6], vUV);
    else if (idx == 7) tex = texture(uTextures[7], vUV);
    else if (idx == 8) tex = texture(uTextures[8], vUV);
    else if (idx == 9) tex = texture(uTextures[9], vUV);
    else if (idx == 10) tex = texture(uTextures[10], vUV);
    else if (idx == 11) tex = texture(uTextures[11], vUV);
    else if (idx == 12) tex = texture(uTextures[12], vUV);
    else if (idx == 13) tex = texture(uTextures[13], vUV);
    else if (idx == 14) tex = texture(uTextures[14], vUV);
    else tex = texture(uTextures[15], vUV);
    FragColor = tex * vColor;
}
)GLSL";

Shader::~Shader() { destroy(); }

Shader::Shader(Shader&& o) noexcept : id_(o.id_), cache_(std::move(o.cache_)) {
    o.id_ = 0;
}
Shader& Shader::operator=(Shader&& o) noexcept {
    if (this != &o) {
        destroy();
        id_ = o.id_; cache_ = std::move(o.cache_);
        o.id_ = 0;
    }
    return *this;
}

static uint32_t compileStage(uint32_t type, const char* src) {
    uint32_t sh = gl::glCreateShader(type);
    gl::glShaderSource(sh, 1, &src, nullptr);
    gl::glCompileShader(sh);
    int ok = 0;
    gl::glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        int len = 0;
        gl::glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        std::string log(len > 0 ? (size_t)len : 512, '\0');
        gl::glGetShaderInfoLog(sh, (int)log.size(), nullptr, log.data());
        FORGE_LOG_ERROR("Shader compile failed: %s", log.c_str());
        gl::glDeleteShader(sh);
        return 0;
    }
    return sh;
}

bool Shader::load(const char* vertSrc, const char* fragSrc) {
    destroy();
    if (!gl::isLoaded() || !vertSrc || !fragSrc) return false;
    uint32_t vs = compileStage(GL_VERTEX_SHADER, vertSrc);
    if (!vs) return false;
    uint32_t fs = compileStage(GL_FRAGMENT_SHADER, fragSrc);
    if (!fs) { gl::glDeleteShader(vs); return false; }

    id_ = gl::glCreateProgram();
    gl::glAttachShader(id_, vs);
    gl::glAttachShader(id_, fs);
    gl::glLinkProgram(id_);
    gl::glDetachShader(id_, vs);
    gl::glDetachShader(id_, fs);
    gl::glDeleteShader(vs);
    gl::glDeleteShader(fs);

    int ok = 0;
    gl::glGetProgramiv(id_, GL_LINK_STATUS, &ok);
    if (!ok) {
        int len = 0;
        gl::glGetProgramiv(id_, GL_INFO_LOG_LENGTH, &len);
        std::string log(len > 0 ? (size_t)len : 512, '\0');
        gl::glGetProgramInfoLog(id_, (int)log.size(), nullptr, log.data());
        FORGE_LOG_ERROR("Program link failed: %s", log.c_str());
        gl::glDeleteProgram(id_);
        id_ = 0;
        return false;
    }
    cache_.clear();
    // Bind texture slots 0..15 once
    bind();
    int slots[16];
    for (int i = 0; i < 16; ++i) slots[i] = i;
    setTextureSlots("uTextures", slots, 16);
    return true;
}

bool Shader::loadFiles(const std::string& vertPath, const std::string& fragPath) {
    std::ifstream vf(vertPath), ff(fragPath);
    if (!vf || !ff) {
        FORGE_LOG_ERROR("Shader file not found: %s / %s", vertPath.c_str(), fragPath.c_str());
        return false;
    }
    std::stringstream vs, fs;
    vs << vf.rdbuf(); fs << ff.rdbuf();
    std::string v = vs.str(), f = fs.str();
    return load(v.c_str(), f.c_str());
}

void Shader::destroy() {
    if (id_ && gl::isLoaded()) gl::glDeleteProgram(id_);
    id_ = 0;
    cache_.clear();
}

void Shader::bind() const {
    if (id_ && gl::isLoaded()) gl::glUseProgram(id_);
}

int Shader::uniformLoc(const char* name) {
    if (!id_ || !gl::isLoaded()) return -1; // post-teardown safe
    auto it = cache_.find(name);
    if (it != cache_.end()) return it->second;
    int loc = (int)gl::glGetUniformLocation(id_, name);
    cache_[name] = loc;
    return loc;
}

void Shader::setInt(const char* name, int v) { gl::glUniform1i(uniformLoc(name), v); }
void Shader::setFloat(const char* name, float v) { gl::glUniform1f(uniformLoc(name), v); }
void Shader::setVec2(const char* name, Vec2 v) { gl::glUniform2f(uniformLoc(name), v.x, v.y); }
void Shader::setVec4(const char* name, float x, float y, float z, float w) {
    gl::glUniform4f(uniformLoc(name), x, y, z, w);
}
void Shader::setMat4(const char* name, const Mat4& m) {
    gl::glUniformMatrix4fv(uniformLoc(name), 1, GL_FALSE, m.data());
}
void Shader::setTextureSlots(const char* name, const int* slots, int count) {
    gl::glUniform1iv(uniformLoc(name), count, slots);
}

} // namespace forge
