cmake_minimum_required(VERSION 3.20)
project(XOR_Engine)

include("${CMAKE_SOURCE_DIR}/cmake/RuntimeDlls.cmake")

# /MP: параллельная компиляция TU ВНУТРИ одного таргета (MSBuild сам параллелит только
# проекты между собой; без /MP ~25 cpp Engine собираются по одному → это и было главным
# множителем «правка одного заголовка = 30 секунд»).
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/MP>")

add_subdirectory(src/engine)
add_subdirectory(src/physics)
add_subdirectory(src/game)
add_subdirectory(src/mygame)
add_subdirectory(src/sandbox)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT Game)
