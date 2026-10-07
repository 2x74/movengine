add_library(bsp STATIC bsp_loader.cpp spawn_point.cpp)
target_include_directories(bsp PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bsp PUBLIC sourcepp::bsppp sourcepp::vtfpp sourcepp::kvpp glm::glm)
