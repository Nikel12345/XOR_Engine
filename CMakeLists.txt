cmake_minimum_required(VERSION 3.20)
project(XOR_Engine)

include("${CMAKE_SOURCE_DIR}/cmake/RuntimeDlls.cmake")

add_subdirectory(src/engine)
add_subdirectory(src/physics)
add_subdirectory(src/game)
add_subdirectory(src/mygame)
add_subdirectory(src/sandbox)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT Game)
