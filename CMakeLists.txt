cmake_minimum_required(VERSION 3.25)
project(movengine CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/W4)
    # Static CRT so the exe runs on a bare Windows box without the VC++ redistributable.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    add_compile_options(-Wall -Wextra)
    # Static libstdc++/libgcc so Linux only needs to resolve ubiquitous system libs at runtime.
    add_link_options(-static-libgcc -static-libstdc++)
endif()

include(FetchContent)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.2.14
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(SDL3)

# SDL only compiles X11 relative-mouse support when libxi's headers are there
# when IT configures, and a missing one is otherwise silent until the game is
# running and swallowing input. Catch it here instead.
if(UNIX AND NOT APPLE AND NOT SDL_X11_XINPUT)
    message(WARNING
        "SDL was configured without XInput2, so mouse capture will not work: "
        "no mouse look and no WASD.\n"
        "  Install the headers and configure again in a clean build folder:\n"
        "    sudo apt install libxi-dev libxfixes-dev libxcursor-dev\n"
        "    ./rebuild.sh --clean")
endif()

# nativefiledialog-extended was the original plan, but its open-dialog args
# struct has no title field at all (confirmed by reading its nfd.h/nfd_gtk.cpp)
# so it can't produce a "choose your map" titled dialog. tinyfiledialogs takes
# a title string directly, and this mirror ships a working CMakeLists.txt
# (target `tinyfiledialogs`, requires CMake >= 3.25).
FetchContent_Declare(
    tinyfiledialogs
    GIT_REPOSITORY https://github.com/polyfem/tinyfiledialogs.git
    GIT_TAG main
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(tinyfiledialogs)

# sourcepp (bsppp) for parsing CS:S .bsp files. bsppp itself transitively
# depends on vpkpp (embedded pak lump) so we leave sourcepp's per-format
# library options at their defaults rather than hand-picking a minimal set.
set(SOURCEPP_BUILD_C_WRAPPERS OFF CACHE BOOL "" FORCE)
set(SOURCEPP_BUILD_CSHARP_WRAPPERS OFF CACHE BOOL "" FORCE)
set(SOURCEPP_BUILD_PYTHON_WRAPPERS OFF CACHE BOOL "" FORCE)
set(SOURCEPP_BUILD_WASM_WRAPPERS OFF CACHE BOOL "" FORCE)
set(SOURCEPP_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(SOURCEPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SOURCEPP_BUILD_TESTS_EXTRA OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    sourcepp
    GIT_REPOSITORY https://github.com/craftablescience/sourcepp.git
    GIT_TAG v2026.9.11
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(sourcepp)

# vtfpp pulls in AMD Compressonator for BCn, and Compressonator's Windows code
# path has only ever been built with MSVC. MinGW-w64 walks into it because it
# defines both _WIN32 and _M_X64 (the latter for MSVC compatibility), so GCC
# compiles Windows-only SIMD that MSVC needs no flags for. Two symptoms:
#
#  - bcn_common_api.h's cmp_floor() takes its SSE4.1 path, and Compressonator
#    passes -msse4.1 only to its own CMP_Core_SSE target, so every other target
#    dies on "inlining failed in call to always_inline _mm_round_ss".
#  - cmp_math_common.cpp's `#ifdef _WIN32` block uses SSE and FMA intrinsics
#    without including a header for them.
#
# Linux GCC sees neither: _M_X64 is undefined there, so cmp_floor() falls back
# to plain floor() and that whole _WIN32 block is never compiled.
#
# -msse4.1 goes on the targets (2008-era hardware, well under what an OpenGL
# 3.3 game needs). -mfma is confined to the one file that needs it rather than
# put on the target, so GCC cannot emit FMA into the encoder kernels that do
# run: nothing outside cmp_math_common.cpp references its sse_*/fma_* helpers,
# so that file's FMA code is unreachable and never executes on a CPU without it.
if(WIN32 AND NOT MSVC)
    foreach(cmp_target CMP_Core CMP_Core_AVX CMP_Core_AVX512 CMP_Compressonator)
        if(TARGET ${cmp_target})
            target_compile_options(${cmp_target} PRIVATE -msse4.1)
        endif()
    endforeach()
    if(TARGET CMP_Core)
        get_target_property(BHOP_CMP_CORE_DIR CMP_Core SOURCE_DIR)
        set_source_files_properties(${BHOP_CMP_CORE_DIR}/source/cmp_math_common.cpp
            DIRECTORY ${BHOP_CMP_CORE_DIR}
            PROPERTIES COMPILE_OPTIONS "-mfma;-include;immintrin.h")
    endif()
endif()

FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glm)

# Dear ImGui ships no CMakeLists of its own (raw sources only), so fetch the
# source and build it ourselves in third_party/.
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.9
    GIT_SHALLOW TRUE
)
FetchContent_Populate(imgui)

# The finished app goes in linux/ or windows/ at the top of the repo: both
# exes side by side (each launches the other) plus the sounds folder (see
# src/app and src/editor).
if(WIN32)
    set(BHOP_PLATFORM windows)
else()
    set(BHOP_PLATFORM linux)
endif()
set(BHOP_APP_DIR ${CMAKE_SOURCE_DIR}/${BHOP_PLATFORM})

# Stamp the build with the commit it came from, so a binary can say what it
# is. Re-run at build time, not just configure: a rebuild after pulling would
# otherwise keep reporting whatever was checked out when CMake last ran.
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
                    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                    OUTPUT_VARIABLE BHOP_GIT_COMMIT
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=no
                    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                    OUTPUT_VARIABLE BHOP_GIT_DIRTY
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    if(NOT BHOP_GIT_DIRTY STREQUAL "")
        set(BHOP_GIT_COMMIT "${BHOP_GIT_COMMIT}-modified")
    endif()
endif()
if(NOT BHOP_GIT_COMMIT)
    set(BHOP_GIT_COMMIT "unknown")
endif()
add_compile_definitions(BHOP_GIT_COMMIT="${BHOP_GIT_COMMIT}")

add_subdirectory(third_party)
add_subdirectory(src)


# `cmake --build build --target release_archive`: the app folder as
# movengine-linux.tar.xz or movengine-windows.zip in the build
# folder, unpacking to a movengine/ folder.
set(BHOP_STAGE ${CMAKE_BINARY_DIR}/release_stage/movengine)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/release_stage)
if(WIN32)
    set(BHOP_ARCHIVE ${CMAKE_BINARY_DIR}/movengine-windows.zip)
    set(BHOP_ARCHIVE_FLAGS cf ${BHOP_ARCHIVE} --format=zip)
else()
    set(BHOP_ARCHIVE ${CMAKE_BINARY_DIR}/movengine-linux.tar.xz)
    set(BHOP_ARCHIVE_FLAGS cJf ${BHOP_ARCHIVE})
endif()
add_custom_target(release_archive
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${BHOP_STAGE}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BHOP_STAGE}
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:movengine> $<TARGET_FILE:movengine_editor> ${BHOP_STAGE}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets/sounds ${BHOP_STAGE}/sounds
    COMMAND ${CMAKE_COMMAND} -E tar ${BHOP_ARCHIVE_FLAGS} -- movengine
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/release_stage
    DEPENDS movengine movengine_editor
    COMMENT "packing ${BHOP_ARCHIVE}"
    VERBATIM)
