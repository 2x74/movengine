add_library(movement STATIC replay.cpp player_move.cpp noclip.cpp styles.cpp timer.cpp triggers.cpp zones.cpp personal_bests.cpp)
target_include_directories(movement PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(movement PUBLIC collision glm::glm)
