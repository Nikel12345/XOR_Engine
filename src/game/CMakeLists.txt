add_executable(Game)

target_sources(Game PRIVATE
    src/main.cpp
    src/Game.cpp
    src/GameShaderSet.cpp
    src/GameComponents.cpp
)

target_include_directories(Game PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(Game PRIVATE Engine Physics)

# PCH: std/SDL/glm (чистый PCH.h) + Engine.h (фасад движка). Правка заголовков движка
# пересобирает PCH один раз, а TU игры дальше грузят его бинарно, а не парсят текст заново.
# Именно PCH.h, а не PCH_Engine.h: последний доклеивает imgui.h, и игра получала бы его
# принудительным инклюдом даже без единой строчки #include — редактор для неё опционален.
target_precompile_headers(Game PRIVATE
    "${CMAKE_SOURCE_DIR}/src/engine/include/PCH.h"
    "${CMAKE_SOURCE_DIR}/src/engine/include/Engine.h"
)

set_target_properties(Game PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/game"
)

copy_runtime_dlls(Game)

# ============================================================
# INSTALL — запускается только cmake --install
# ============================================================

# Сам exe
install(TARGETS Game
    RUNTIME DESTINATION .       # Game.exe ляжет в корень dist/
)

# DLL-ки. SDL3.dll теперь собирается нами (external/SDL3), поэтому берём её у цели,
# а не по фиксированному пути; остальные — готовые бинарники.
install(FILES
    $<TARGET_FILE:SDL3::SDL3>
    "${CMAKE_SOURCE_DIR}/external/SDL3_image/lib/x64/SDL3_image.dll"
    DESTINATION .
)

# Ресурсы игры
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/models/"
    DESTINATION models
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/textures/"
    DESTINATION textures
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/sounds/"
    DESTINATION sounds
)

# Шейдеры движка
install(DIRECTORY "${CMAKE_SOURCE_DIR}/src/engine/shaders/"
    DESTINATION ../engine/shaders
)