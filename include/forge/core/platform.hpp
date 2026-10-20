#pragma once
// Platform detection. Every macro is always defined to 0 or 1 so
// `#if FORGE_PLATFORM_WEB` compiles identically everywhere.

#if defined(__EMSCRIPTEN__)
#define FORGE_PLATFORM_WEB 1
#else
#define FORGE_PLATFORM_WEB 0
#endif

#if FORGE_PLATFORM_WEB
#define FORGE_PLATFORM_WINDOWS 0
#define FORGE_PLATFORM_MACOS 0
#define FORGE_PLATFORM_LINUX 0
#else
#if defined(_WIN32)
#define FORGE_PLATFORM_WINDOWS 1
#else
#define FORGE_PLATFORM_WINDOWS 0
#endif
#if defined(__APPLE__)
#define FORGE_PLATFORM_MACOS 1
#else
#define FORGE_PLATFORM_MACOS 0
#endif
#if defined(__linux__)
#define FORGE_PLATFORM_LINUX 1
#else
#define FORGE_PLATFORM_LINUX 0
#endif
#endif
