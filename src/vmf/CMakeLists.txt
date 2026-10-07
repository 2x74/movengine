add_library(vmf STATIC document.cpp geometry.cpp playtest.cpp bsp_import.cpp bsp_write.cpp bsp_compile.cpp)
target_include_directories(vmf PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(vmf PUBLIC collision bsp platform glm::glm)
