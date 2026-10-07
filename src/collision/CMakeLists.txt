add_library(collision STATIC brush_polygons.cpp bsp_brush_loader.cpp clip.cpp map_entities.cpp)
target_include_directories(collision PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(collision PUBLIC sourcepp::bsppp glm::glm)
