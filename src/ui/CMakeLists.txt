add_library(ui STATIC imgui_layer.cpp menu_bar.cpp keybind_editor.cpp keybinds.cpp settings_panel.cpp
    strafe_trainer.cpp run_hud.cpp jhud.cpp zones_window.cpp sounds_window.cpp)
target_include_directories(ui PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ui PUBLIC imgui platform console movement audio)
