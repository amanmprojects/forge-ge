cmake_minimum_required(VERSION 3.20)
project(forge VERSION 0.2.0 LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

# Options (FORGE_ENABLE_PROFILING drives the FORGE_PROFILING macro, always
# defined to 0/1 so `#if FORGE_PROFILING` works without warnings)
option(FORGE_INSTALL "Generate install/export rules" ON)
option(FORGE_BUILD_EXAMPLES "Build examples" ON)
option(FORGE_BUILD_TESTS "Build tests" ON)
option(FORGE_ENABLE_PROFILING "Enable internal profiler" ON)

# Sanitizers: single-config Debug builds get ASan+UBSan by default (previous
# behavior); set FORGE_ENABLE_SANITIZERS explicitly with multi-config
# generators (CMAKE_BUILD_TYPE is ignored there). The web build has no
# sanitizer runtime.
if(NOT DEFINED FORGE_ENABLE_SANITIZERS)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT CMAKE_CONFIGURATION_TYPES AND NOT EMSCRIPTEN)
        set(FORGE_ENABLE_SANITIZERS ON)
    else()
        set(FORGE_ENABLE_SANITIZERS OFF)
    endif()
endif()
option(FORGE_ENABLE_SANITIZERS "Compile and link with ASan+UBSan" ${FORGE_ENABLE_SANITIZERS})

# Shared warning set: the library, examples and tests are all held to it.
set(FORGE_WARNING_FLAGS -Wall -Wextra -Wpedantic)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Clang-only warnings the reference GCC toolchain never emits; the audio
    # mixer still carries a few dead fade fields.
    list(APPEND FORGE_WARNING_FLAGS -Wno-unused-private-field)
endif()

# Dependencies: SDL3 + OpenGL (system). Everything else is vendored.
if(EMSCRIPTEN)
    # Web build: SDL3 comes from Emscripten's port, GL is the browser's WebGL2
    # (== OpenGL ES 3.0), and there is no m/dl/pthread (single-threaded; the
    # chess bot thinks inline instead of on a worker). See web/README.md.
    set(FORGE_WEB_FLAGS
        -sUSE_SDL=3
        -sMIN_WEBGL_VERSION=2
        -sMAX_WEBGL_VERSION=2
        -sALLOW_MEMORY_GROWTH=1
        -sSTACK_SIZE=2097152
        -sENVIRONMENT=web
        -sEXIT_RUNTIME=0
    )
else()
    # Prefer a CMake SDL3 package, fall back to pkg-config.
    find_package(SDL3 QUIET)
    if(TARGET SDL3::SDL3)
        set(FORGE_SDL_TARGET SDL3::SDL3)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL3 REQUIRED sdl3)
    endif()
    find_package(OpenGL REQUIRED)
endif()

# ---- Engine library (explicit sources: no glob surprises) ----
set(FORGE_SOURCES
    src/core/engine.cpp
    src/core/input.cpp
    src/core/logger.cpp
    src/core/time.cpp
    src/core/window.cpp
    src/graphics/animation.cpp
    src/graphics/atlas.cpp
    src/graphics/camera.cpp
    src/graphics/font.cpp
    src/graphics/framebuffer.cpp
    src/graphics/gl_loader.cpp
    src/graphics/lighting.cpp
    src/graphics/particles.cpp
    src/graphics/postprocess.cpp
    src/graphics/shader.cpp
    src/graphics/skeleton.cpp
    src/graphics/sprite_batch.cpp
    src/graphics/texture.cpp
    src/graphics/tilemap.cpp
    src/physics/physics.cpp
    src/scene/scene.cpp
    src/scene/serialization.cpp
    src/script/script.cpp
    src/assets/assets.cpp
    src/audio/audio.cpp
    src/utils/debug_overlay.cpp
    src/utils/profiler.cpp
)

add_library(forge STATIC ${FORGE_SOURCES})

# Public headers live under include/; third_party is implementation-only
# (stb/miniaudio are compiled into the library, never included by public
# headers), so it must not leak into the install interface.
target_include_directories(forge
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${OPENGL_INCLUDE_DIRS}
)
# Vendored headers stay available to in-tree examples/tests (they use
# stb_image_write for --shot) but are not part of the install interface, and
# they are not held to the project's warning set: miniaudio's web backend in
# particular carries emscripten-only identifiers that clang flags.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.25)
    target_include_directories(forge SYSTEM PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party>)
else()
    target_include_directories(forge PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party>)
endif()
if(DEFINED FORGE_SDL_TARGET)
    target_link_libraries(forge PUBLIC ${FORGE_SDL_TARGET})
elseif(NOT EMSCRIPTEN)
    target_include_directories(forge PUBLIC ${SDL3_INCLUDE_DIRS})
    target_compile_options(forge PUBLIC ${SDL3_CFLAGS_OTHER})
    target_link_directories(forge PUBLIC ${SDL3_LIBRARY_DIRS})
    target_link_libraries(forge PUBLIC ${SDL3_LIBRARIES})
endif()
if(EMSCRIPTEN)
    # -sUSE_SDL=3 has to be a compile flag as well: the driver adds the port's
    # SDL3 headers to the include path. The rest are linker settings.
    target_compile_options(forge PUBLIC -sUSE_SDL=3)
    target_link_options(forge PUBLIC ${FORGE_WEB_FLAGS})
else()
    target_link_libraries(forge PUBLIC
        OpenGL::GL
        m
        pthread
        dl
    )
endif()

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
        examples/10_lantern.cpp
    )
    foreach(src ${FORGE_EXAMPLES})
        get_filename_component(name "${src}" NAME_WE)
        add_executable("${name}" "${src}")
        target_link_libraries("${name}" PRIVATE forge)
        target_compile_options("${name}" PRIVATE ${FORGE_WARNING_FLAGS})
    endforeach()

    # 09_chess lives in its own folder: the rules engine and the bot are
    # header-only, engine-free headers shared with tests/test_chess.cpp.
    add_executable(09_chess examples/chess/main.cpp)
    target_link_libraries(09_chess PRIVATE forge)
    target_include_directories(09_chess PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/examples/chess")
    target_compile_options(09_chess PRIVATE ${FORGE_WARNING_FLAGS})
    if(EMSCRIPTEN)
        # Web page: index.html + index.js + index.wasm + index.data (the
        # preloaded piece sprites, font and SVGs). Serve the folder over HTTP.
        set(FORGE_WEB_DIST "${CMAKE_SOURCE_DIR}/web/dist"
            CACHE PATH "Output folder for the web build (index.html & friends)")
        target_link_options(09_chess PRIVATE
            "--shell-file=${CMAKE_SOURCE_DIR}/web/shell.html"
            "--preload-file=${CMAKE_SOURCE_DIR}/examples/chess/assets@examples/chess/assets"
        )
        set_target_properties(09_chess PROPERTIES
            OUTPUT_NAME "index"
            SUFFIX ".html"
            RUNTIME_OUTPUT_DIRECTORY "${FORGE_WEB_DIST}"
            # The page shell is a link input, not a compiled one: relink when it
            # changes so web/dist picks the edit up.
            LINK_DEPENDS "${CMAKE_SOURCE_DIR}/web/shell.html"
        )
    endif()
endif()

# ---- Install / export (find_package(forge)) ----
if(FORGE_INSTALL)
    install(TARGETS forge
        EXPORT forgeTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    # Public headers only. third_party/ is implementation detail (stb/miniaudio
    # are compiled into the static library, never included by public headers).
    install(DIRECTORY include/forge DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT forgeTargets
        FILE forgeTargets.cmake
        NAMESPACE forge::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/forge
    )
    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/forgeConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/forgeConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/forge
    )
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/forgeConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMinorVersion
    )
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/forgeConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/forgeConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/forge
    )
endif()

# ---- Tests ----
if(FORGE_BUILD_TESTS AND NOT EMSCRIPTEN)
    enable_testing()
    add_executable(test_engine tests/test_engine.cpp)
    target_link_libraries(test_engine PRIVATE forge)
    target_compile_options(test_engine PRIVATE ${FORGE_WARNING_FLAGS})
    add_test(NAME test_engine COMMAND test_engine)

    # Headless chess rules/perft/AI suite — no forge dependency by design.
    # Documentation/API drift guard: compile-time signature checks + file checks.
    add_executable(test_docs tests/test_docs.cpp)
    target_link_libraries(test_docs PRIVATE forge)
    target_compile_options(test_docs PRIVATE ${FORGE_WARNING_FLAGS})
    target_compile_definitions(test_docs PRIVATE
        FORGE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME test_docs COMMAND test_docs)

    add_executable(test_chess tests/test_chess.cpp)
    target_include_directories(test_chess PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_options(test_chess PRIVATE ${FORGE_WARNING_FLAGS})
    add_test(NAME test_chess COMMAND test_chess)
endif()
