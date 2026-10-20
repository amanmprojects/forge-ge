#include "forge/graphics/gl_loader.hpp"
#include "forge/core/logger.hpp"
#include "forge/core/platform.hpp"

#if FORGE_PLATFORM_WEB
// On the web GL is linked into the module (GLES3 via WebGL2), so bind the real
// symbols directly: SDL_GL_GetProcAddress would need the proxy runtime
// (-sGL_ENABLE_GET_PROC_ADDRESS) and report anything not exported as missing.
// The declarations below use the loader's own typedefs; the C names match.
extern "C" {
#define X(name, ret, args) extern ret name args;
FORGE_GL_LIST
#undef X
}
#endif

namespace forge::gl {

#define X(name, ret, args) name##Proc name = nullptr;
FORGE_GL_LIST
#undef X

static bool s_loaded = false;

bool load(ProcLoader loader) {
    int missing = 0;
#if FORGE_PLATFORM_WEB
    (void)loader;
#define X(name, ret, args) name = ::name;
    FORGE_GL_LIST
#undef X
#else
    if (!loader) return false;
#define X(name, ret, args) \
    name = (name##Proc)loader(#name); \
    if (!name) { ++missing; FORGE_LOG_ERROR("GL: missing symbol %s", #name); }
    FORGE_GL_LIST
#undef X
#endif
    if (missing > 0)
        FORGE_LOG_ERROR("GL: %d required symbol(s) missing; GL support disabled", missing);
    s_loaded = (missing == 0);
    return s_loaded;
}

bool isLoaded() { return s_loaded; }

} // namespace forge::gl
