#include "forge/graphics/gl_loader.hpp"

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
    if (!name) ++missing;
    FORGE_GL_LIST
#undef X
    s_loaded = (missing == 0);
    return s_loaded;
}

bool isLoaded() { return s_loaded; }

} // namespace forge::gl
