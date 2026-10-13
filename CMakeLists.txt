cmake_minimum_required(VERSION 3.20)
project(forge VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

# Options
option(FORGE_BUILD_EXAMPLES "Build examples" ON)
option(FORGE_BUILD_TESTS "Build tests" ON)
option(FORGE_ENABLE_PROFILING "Enable internal profiler" ON)

# Dependencies: SDL3 + OpenGL (system). Everything else is vendored.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED sdl3)
find_package(OpenGL REQUIRED)

# ---- Engine library ----
file(GLOB_RECURSE FORGE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
)

add_library(forge STATIC ${FORGE_SOURCES})

target_include_directories(forge PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
    ${SDL3_INCLUDE_DIRS}
    ${OPENGL_INCLUDE_DIRS}
)

target_compile_options(forge PUBLIC ${SDL3_CFLAGS_OTHER})
target_link_directories(forge PUBLIC ${SDL3_LIBRARY_DIRS})
target_link_libraries(forge PUBLIC
    ${SDL3_LIBRARIES}
    OpenGL::GL
    m
    pthread
    dl
)

target_compile_options(forge PRIVATE -Wall -Wextra -Wpedantic -O3)

if(FORGE_ENABLE_PROFILING)
    target_compile_definitions(forge PUBLIC FORGE_PROFILING=1)
endif()

# Strict in debug
target_compile_options(forge PRIVATE
    $<$<CONFIG:Debug>:-g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer>
)
target_link_options(forge PUBLIC
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)

# ---- Examples ----
if(FORGE_BUILD_EXAMPLES)
    file(GLOB EXAMPLE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/examples/*.cpp")
    foreach(src ${EXAMPLE_SOURCES})
        get_filename_component(name "${src}" NAME_WE)
        add_executable("${name}" "${src}")
        target_link_libraries("${name}" PRIVATE forge)
        target_compile_options("${name}" PRIVATE -Wall -Wextra -O2)
    endforeach()
endif()

# ---- Tests ----
if(FORGE_BUILD_TESTS)
    enable_testing()
    file(GLOB TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
    foreach(src ${TEST_SOURCES})
        get_filename_component(name "${src}" NAME_WE)
        add_executable("${name}" "${src}")
        target_link_libraries("${name}" PRIVATE forge)
        target_compile_options("${name}" PRIVATE -Wall -Wextra -O2)
        add_test(NAME "${name}" COMMAND "${name}")
    endforeach()
endif()
