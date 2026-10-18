cmake_minimum_required(VERSION 3.20)
project(forge VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

# Options (FORGE_ENABLE_PROFILING drives the FORGE_PROFILING macro, always
# defined to 0/1 so `#if FORGE_PROFILING` works without warnings)
option(FORGE_BUILD_EXAMPLES "Build examples" ON)
option(FORGE_BUILD_TESTS "Build tests" ON)
option(FORGE_ENABLE_PROFILING "Enable internal profiler" ON)

# Sanitizers: single-config Debug builds get ASan+UBSan by default (previous
# behavior); set FORGE_ENABLE_SANITIZERS explicitly with multi-config
# generators (CMAKE_BUILD_TYPE is ignored there).
if(NOT DEFINED FORGE_ENABLE_SANITIZERS)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT CMAKE_CONFIGURATION_TYPES)
        set(FORGE_ENABLE_SANITIZERS ON)
    else()
        set(FORGE_ENABLE_SANITIZERS OFF)
    endif()
endif()
option(FORGE_ENABLE_SANITIZERS "Compile and link with ASan+UBSan" ${FORGE_ENABLE_SANITIZERS})

# Shared warning set: the library, examples and tests are all held to it.
set(FORGE_WARNING_FLAGS -Wall -Wextra -Wpedantic)

# Dependencies: SDL3 + OpenGL (system). Everything else is vendored.
# Prefer a CMake SDL3 package, fall back to pkg-config.
find_package(SDL3 QUIET)
if(TARGET SDL3::SDL3)
    set(FORGE_SDL_TARGET SDL3::SDL3)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL3 REQUIRED sdl3)
endif()
find_package(OpenGL REQUIRED)

# ---- Engine library (explicit sources: no glob surprises) ----
set(FORGE_SOURCES
    src/core/engine.cpp
    src/core/input.cpp
    src/core/logger.cpp
    src/core/time.cpp
    src/core/window.cpp
    src/graphics/animation.cpp
    src/graphics/camera.cpp
    src/graphics/font.cpp
    src/graphics/framebuffer.cpp
    src/graphics/gl_loader.cpp
    src/graphics/particles.cpp
    src/graphics/shader.cpp
    src/graphics/sprite_batch.cpp
    src/graphics/texture.cpp
    src/graphics/tilemap.cpp
    src/physics/physics.cpp
    src/scene/scene.cpp
    src/assets/assets.cpp
    src/audio/audio.cpp
    src/utils/profiler.cpp
)

add_library(forge STATIC ${FORGE_SOURCES})

target_include_directories(forge PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
    ${OPENGL_INCLUDE_DIRS}
)
if(DEFINED FORGE_SDL_TARGET)
    target_link_libraries(forge PUBLIC ${FORGE_SDL_TARGET})
else()
    target_include_directories(forge PUBLIC ${SDL3_INCLUDE_DIRS})
    target_compile_options(forge PUBLIC ${SDL3_CFLAGS_OTHER})
    target_link_directories(forge PUBLIC ${SDL3_LIBRARY_DIRS})
    target_link_libraries(forge PUBLIC ${SDL3_LIBRARIES})
endif()
target_link_libraries(forge PUBLIC
    OpenGL::GL
    m
    pthread
    dl
)

# The version macros live in forge.hpp with fallback defaults; CMake (the
# real project() version) overrides them via compile definitions.
target_compile_definitions(forge PUBLIC
    FORGE_PROFILING=$<IF:$<BOOL:${FORGE_ENABLE_PROFILING}>,1,0>
    FORGE_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    FORGE_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    FORGE_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    FORGE_VERSION="${PROJECT_VERSION}"
)

target_compile_options(forge PRIVATE ${FORGE_WARNING_FLAGS})
target_compile_options(forge PRIVATE $<$<CONFIG:Release>:-O3>)

# Sanitizers (compile AND link, symmetric — the old setup linked PUBLIC but
# compiled PRIVATE, breaking downstream ASan builds).
if(FORGE_ENABLE_SANITIZERS)
    target_compile_options(forge PUBLIC -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(forge PUBLIC -fsanitize=address,undefined)
endif()

# ---- Examples ----
if(FORGE_BUILD_EXAMPLES)
    set(FORGE_EXAMPLES
        examples/01_hello.cpp
        examples/02_platformer.cpp
        examples/03_benchmark.cpp
        examples/04_audio.cpp
        examples/05_scene.cpp
        examples/06_verify.cpp
        examples/07_breakout.cpp
        examples/08_snake.cpp
    )
    foreach(src ${FORGE_EXAMPLES})
        get_filename_component(name "${src}" NAME_WE)
        add_executable("${name}" "${src}")
        target_link_libraries("${name}" PRIVATE forge)
        target_compile_options("${name}" PRIVATE ${FORGE_WARNING_FLAGS})
    endforeach()
endif()

# ---- Tests ----
if(FORGE_BUILD_TESTS)
    enable_testing()
    add_executable(test_engine tests/test_engine.cpp)
    target_link_libraries(test_engine PRIVATE forge)
    target_compile_options(test_engine PRIVATE ${FORGE_WARNING_FLAGS})
    add_test(NAME test_engine COMMAND test_engine)
endif()
