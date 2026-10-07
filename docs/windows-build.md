# Building for Windows

There are two ways to get `windows/movengine.exe` and
`windows/movengine_editor.exe`. Both drop the two exes plus a `sounds/` folder into
`windows/` at the top of the repo, which is what the game and the editor expect
when they launch each other.

The exes are statically linked either way, so they run on a Windows box with
nothing installed -- no Visual C++ redistributable, no MinGW DLLs beside them.

## On Windows, with Visual Studio

Install "Desktop development with C++" (Visual Studio 2022 or the standalone
Build Tools) and Git, then from the repo root:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
```

The first configure clones SDL3, sourcepp, glm, tinyfiledialogs and Dear ImGui
through FetchContent -- a few GB and a long wait. Later builds reuse `build/`.

Nothing else to install: SDL talks to Win32, WASAPI and OpenGL directly, and
the file dialogs come from comdlg32 via tinyfiledialogs.

## From Linux, cross-compiling with MinGW-w64

Useful when Linux is what you have in front of you. Install the toolchain:

```
sudo apt install mingw-w64            # Debian/Ubuntu
sudo dnf install mingw64-gcc-c++      # Fedora
```

Then either let `rebuild.sh` do it:

```
./rebuild.sh --windows
```

or drive CMake yourself, which is the same thing:

```
cmake -S . -B build-windows -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
cmake --build build-windows -j
```

The cross build keeps its own `build-windows/` tree. Sharing `build/` with the
Linux build does not work -- CMake caches the compiler, and the fetched
dependencies are configured for one target or the other, not both.

### Two things the toolchain file handles

Worth knowing, because the failures are obscure:

- **Threads.** Ubuntu's unsuffixed `x86_64-w64-mingw32-g++` is the `win32`
  threading variant, whose libstdc++ leaves `std::thread`, `std::mutex` and
  `std::condition_variable` undefined. The only symptom is a link failure in
  `src/app/main.cpp`. The toolchain file asks for `-posix` by name.
- **Header case.** Windows code spells SDK headers in mixed case
  (`<Windows.h>`, `<ShlObj.h>`); MinGW-w64 ships them lowercase, and on a
  case-sensitive filesystem that is a plain "No such file or directory" --
  sourcepp's gamepp hits it first. `cmake/mingw-include/` holds forwarding
  headers under the mixed-case names and is searched ahead of the sysroot.

### Testing it

There is no Windows machine in a cross build, so the exes are untested when
they come out. `wine windows/movengine.exe bhop_icetrap.bsp` is a decent
smoke test if you have Wine; otherwise copy `windows/` to a Windows box.

## Packaging

```
cmake --build build-windows --target release_archive
```

writes `build-windows/movengine-windows.zip`, unpacking to a
`movengine/` folder with both exes and the sounds.

## Optional runtime extra

Video recording shells out to `ffmpeg`, so starting a recording without
`ffmpeg.exe` on `PATH` reports "ffmpeg not found on PATH" instead of recording.
`winget install ffmpeg` is enough. Nothing else about the build depends on it.
