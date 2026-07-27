add_executable(Sandbox)

# Площадки для запуска подсистем движка по отдельности.
# Каждый сценарий — свой .cpp; main решает, что именно запустить.
set(SANDBOX_SRC
    src/CopyComputeBarrierProbe.cpp   # зонд: порядок нескольких операций над одним буфером в одном cb, без fence (см. шапку файла)
    # src/main.cpp              # зонд: TDM store под контеншеном памяти
    # src/SceneJsonProbe.cpp    # зонд: round-trip scene.json (раскладка файла + сохранность данных), без GPU
    # src/BlitUsageProbe.cpp    # зонд: какие usage-флаги требуют blit/copy/mip-gen (см. шапку файла)
    # src/CameraScene.cpp
    # src/BufferScene.cpp
)

target_sources(Sandbox PRIVATE ${SANDBOX_SRC})
source_group("Source" FILES ${SANDBOX_SRC})

# Свои заголовки песочницы. Публичный API движка и его PUBLIC-зависимости
# (glm, SDL3_image, а после правки в engine — и SDL3) приходят транзитивно от Engine.
target_include_directories(Sandbox PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(Sandbox PRIVATE Engine)

# PCH: только std/SDL/glm — Sandbox дергает подсистемы точечно, Engine.h ему не нужен.
target_precompile_headers(Sandbox PRIVATE
    "${CMAKE_SOURCE_DIR}/src/engine/include/PCH.h"
)

# Рабочая директория при отладке — откуда грузятся ресурсы/шейдеры.
# Поставь туда, где лежат ассеты сценариев. Если переиспользуешь ассеты игры —
# оставь путь к src/game; если у песочницы свои — поменяй на свой каталог.
set_target_properties(Sandbox PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/game"
)

# DLL рядом с exe, иначе Sandbox.exe соберётся, но не запустится.
copy_runtime_dlls(Sandbox)