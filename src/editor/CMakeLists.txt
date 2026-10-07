add_executable(movengine_editor main.cpp editor.cpp)
target_link_libraries(movengine_editor PRIVATE platform render ui vmf collision content)
# Same folder as the game, so each can find and launch the other.
set_target_properties(movengine_editor PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${BHOP_APP_DIR}$<0:>")
if(NOT MSVC)
    target_link_options(movengine_editor PRIVATE $<$<CONFIG:Release,MinSizeRel>:-s>)
endif()
