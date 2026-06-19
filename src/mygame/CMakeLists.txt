add_executable(MyGame)

target_sources(MyGame PRIVATE
    src/main.cpp
    src/MyGame.cpp
)

target_include_directories(MyGame PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(MyGame PRIVATE Engine Physics)

# Рабочая директория при отладке — откуда грузятся ресурсы/шейдеры.
# Свои ассеты у mygame ещё не заведены, поэтому временно указываем на каталог
# игры (там лежат textures/models/sounds и движковые шейдеры подтянутся как у Game).
# Когда появятся свои ассеты — переставь на ${CMAKE_CURRENT_SOURCE_DIR}.
set_target_properties(MyGame PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/game"
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
