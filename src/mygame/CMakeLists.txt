add_executable(MyGame)

target_sources(MyGame PRIVATE
    src/main.cpp
    src/MyGame.cpp
    src/DefaultShaderSet.cpp
)

target_include_directories(MyGame PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(MyGame PRIVATE Engine Physics)

# PCH: как у Game — std/SDL/glm/imgui + Engine.h (см. src/game/CMakeLists.txt).
target_precompile_headers(MyGame PRIVATE
    "${CMAKE_SOURCE_DIR}/src/engine/include/PCH_Engine.h"
    "${CMAKE_SOURCE_DIR}/src/engine/include/Engine.h"
)

# Рабочая директория при отладке — откуда грузятся ресурсы/шейдеры.
# Свои ассеты mygame лежат рядом (models/, textures/); движковые шейдеры
# грузятся по относительному "../engine/shaders_code/..." и из этого каталога
# тоже резолвятся (../engine = src/engine).
set_target_properties(MyGame PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

copy_runtime_dlls(MyGame)

# ============================================================
# INSTALL — запускается только cmake --install
# ============================================================

# Сам exe
install(TARGETS MyGame
    RUNTIME DESTINATION .       # MyGame.exe ляжет в корень dist/
)

# DLL-ки
install(FILES
    "${CMAKE_SOURCE_DIR}/external/SDL3/bin/SDL3.dll"
    "${CMAKE_SOURCE_DIR}/external/SDL3_image/lib/x64/SDL3_image.dll"
    DESTINATION .
)

# Шейдеры движка
install(DIRECTORY "${CMAKE_SOURCE_DIR}/src/engine/shaders/"
    DESTINATION ../engine/shaders
)

# Ресурсы игры — раскомментируй и заведи каталоги, когда появятся свои ассеты:
# install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/models/"   DESTINATION models)
# install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/textures/" DESTINATION textures)
# install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/sounds/"   DESTINATION sounds)
