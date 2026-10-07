add_executable(movengine main.cpp)
target_link_libraries(movengine PRIVATE platform bsp collision console movement render ui vmf content audio)
set_target_properties(movengine PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${BHOP_APP_DIR}$<0:>")
if(NOT MSVC)
    target_link_options(movengine PRIVATE $<$<CONFIG:Release,MinSizeRel>:-s>)
endif()
add_custom_command(TARGET movengine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets/sounds ${BHOP_APP_DIR}/sounds)
# Builds before the linux/ / windows/ folders put the exes here; remove those so an old copy can't be run by mistake.
file(REMOVE ${CMAKE_BINARY_DIR}/src/app/movengine ${CMAKE_BINARY_DIR}/src/app/movengine_editor
     ${CMAKE_BINARY_DIR}/src/app/movengine.exe ${CMAKE_BINARY_DIR}/src/app/movengine_editor.exe)
