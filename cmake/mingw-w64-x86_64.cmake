# Cross-compile the Windows build from Linux with MinGW-w64.
#
#   cmake -S . -B build-windows -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
#
# Debian/Ubuntu: sudo apt install mingw-w64
# Fedora:        sudo dnf install mingw64-gcc-c++
#
# Building on Windows itself needs none of this -- plain `cmake -S . -B build`
# with Visual Studio picks up the MSVC branches in the CMake files instead.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(BHOP_MINGW_PREFIX x86_64-w64-mingw32)

# Prefer the -posix variants, whose libstdc++ links threading against
# winpthreads. On Debian/Ubuntu the unsuffixed x86_64-w64-mingw32-g++ is the
# -win32 variant, which leaves std::thread, std::mutex and
# std::condition_variable as undefined symbols -- and the only sign of it is a
# link failure in src/app/main.cpp. Fedora and Arch ship no -posix suffix and
# are already posix-threaded, so fall back to the plain name there.
find_program(BHOP_MINGW_CC NAMES ${BHOP_MINGW_PREFIX}-gcc-posix ${BHOP_MINGW_PREFIX}-gcc REQUIRED)
find_program(BHOP_MINGW_CXX NAMES ${BHOP_MINGW_PREFIX}-g++-posix ${BHOP_MINGW_PREFIX}-g++ REQUIRED)
find_program(BHOP_MINGW_RC NAMES ${BHOP_MINGW_PREFIX}-windres)
set(CMAKE_C_COMPILER ${BHOP_MINGW_CC})
set(CMAKE_CXX_COMPILER ${BHOP_MINGW_CXX})
if(BHOP_MINGW_RC)
    set(CMAKE_RC_COMPILER ${BHOP_MINGW_RC})
endif()

# Fully static: the exe carries libstdc++, libgcc and winpthread, so it runs
# on a Windows box with nothing installed -- same goal as the static CRT the
# MSVC branch asks for.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")

# Look for headers and libraries in the cross sysroot, never in /usr, but keep
# running host programs (git, the archiver) from the host PATH.
set(CMAKE_FIND_ROOT_PATH /usr/${BHOP_MINGW_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Windows code spells the SDK headers in mixed case (<Windows.h>, <ShlObj.h>),
# MinGW-w64 ships them lowercase, and a case-sensitive filesystem turns that
# into "No such file or directory" -- sourcepp's gamepp hits it first. This
# folder holds forwarding headers under the mixed-case names; it is searched
# before the sysroot and contains nothing else, so the real headers still come
# from MinGW. Harmless on Windows itself, where the filesystem does not care.
set(BHOP_MINGW_CASE_SHIMS ${CMAKE_CURRENT_LIST_DIR}/mingw-include)
string(APPEND CMAKE_C_FLAGS_INIT " -isystem ${BHOP_MINGW_CASE_SHIMS}")
string(APPEND CMAKE_CXX_FLAGS_INIT " -isystem ${BHOP_MINGW_CASE_SHIMS}")
