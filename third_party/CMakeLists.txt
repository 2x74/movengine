# Vendored third-party code.
#
# glad/ is a generated GL 3.3 core loader (via the `glad2` pip generator,
# `glad --api gl:core=3.3 --extensions "" c`), checked in rather than
# regenerated at build time so building this project needs no Python.

add_library(glad STATIC glad/src/gl.c)
target_include_directories(glad PUBLIC glad/include)
target_link_libraries(glad PUBLIC ${CMAKE_DL_LIBS})

# tinyfiledialogs (target `tinyfiledialogs`, alias `tinyfd::tinyfd`) is fetched
# via FetchContent_MakeAvailable in the top-level CMakeLists.txt — it ships its
# own CMakeLists.txt, nothing to add here. On Linux it shells out to
# zenity/kdialog/python-tk at runtime rather than linking a toolkit.

# Dear ImGui: fetched via FetchContent_Populate (no CMake of its own), built
# here with the SDL3 + OpenGL3 backends since that's our window/GL stack.
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3 glad)
