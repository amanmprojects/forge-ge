#include "forge/graphics/gl_loader.hpp"
#include "forge/core/logger.hpp"

namespace forge::gl {

#define X(name, ret, args) name##Proc name = nullptr;
FORGE_GL_LIST
#undef X

static bool s_loaded = false;

bool load(ProcLoader loader) {
    if (!loader) return false;
    int missing = 0;
#define X(name, ret, args) \
    name = (name##Proc)loader(#name); \
    if (!name) { ++missing; FORGE_LOG_ERROR("GL: missing symbol %s", #name); }
    FORGE_GL_LIST
#undef X
    if (missing > 0)
        FORGE_LOG_ERROR("GL: %d required symbol(s) missing; GL support disabled", missing);
    s_loaded = (missing == 0);
    return s_loaded;
}

bool isLoaded() { return s_loaded; }

} // namespace forge::gl
