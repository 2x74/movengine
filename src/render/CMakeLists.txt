add_library(render STATIC shader.cpp mesh.cpp camera.cpp texture.cpp debug_box.cpp lighting.cpp)
target_include_directories(render PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(render PUBLIC glad glm::glm bsp)
