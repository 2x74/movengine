add_library(content STATIC builtin_textures.cpp game_content.cpp materials.cpp zip_writer.cpp)
target_include_directories(content PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(content PUBLIC bsp glm::glm sourcepp::bsppp sourcepp::kvpp sourcepp::vpkpp sourcepp::vtfpp sourcepp::steampp)
