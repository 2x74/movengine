add_library(platform STATIC video_recorder.cpp window.cpp file_dialog.cpp process.cpp user_files.cpp discord.cpp)
target_include_directories(platform PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(platform PUBLIC SDL3::SDL3 glad tinyfiledialogs)
