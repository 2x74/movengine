add_library(console STATIC cvar.cpp cfg_exec.cpp)
target_include_directories(console PUBLIC ${CMAKE_SOURCE_DIR}/src)
