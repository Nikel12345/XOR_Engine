# ============================================================
#  Внешние под-библиотеки
# ============================================================

# ImGui — нужен только верхнему слою (Engine: UI_ImGui.cpp, Engine.cpp)
add_library(ImGui STATIC
    "${CMAKE_SOURCE_DIR}/external/ImGui/src/imgui.cpp"
    "${CMAKE_SOURCE_DIR}/external/ImGui/src/imgui_draw.cpp"
    "${CMAKE_SOURCE_DIR}/external/ImGui/src/imgui_tables.cpp"
    "${CMAKE_SOURCE_DIR}/external/ImGui/src/imgui_widgets.cpp"
    "${CMAKE_SOURCE_DIR}/external/ImGui/src/imgui_impl_sdl3.cpp"
    "${CMAKE_SOURCE_DIR}/external/ImGui/src/imgui_impl_sdlgpu3.cpp"
    # ImGuizmo — гизмо трансформации поверх ImGui (один cpp, тянет imgui_internal.h)
    "${CMAKE_SOURCE_DIR}/external/ImGuizmo/ImGuizmo.cpp"
)
target_include_directories(ImGui PUBLIC
    "${CMAKE_SOURCE_DIR}/external/ImGui/include"
    "${CMAKE_SOURCE_DIR}/external/ImGuizmo"
)
target_link_libraries(ImGui PUBLIC
    SDL3::SDL3          # include-пути SDL приходят вместе с целью (см. корневой CMakeLists)
)

# SPIRV (spirv_reflect) удалён из сборки: рефлексия идёт через shadercross
# (SDL_ShaderCross_Reflect*), а SpvReflect* в коде не вызывается. Файлы в
# external/SPIRV оставлены на месте — при необходимости вернуть таргет сюда.

# yyjson — вендорный JSON (файлы сцены/ресурсов: Engine::Save/LoadScene). C-либа из одного
# .c/.h (external/yyjson, MIT). Линкуется только Engine (PRIVATE): json — деталь файлового IO,
# наружу торчат лишь Save/LoadScene(папка).
add_library(yyjson STATIC
    "${CMAKE_SOURCE_DIR}/external/yyjson/yyjson.c"
)
target_include_directories(yyjson PUBLIC
    "${CMAKE_SOURCE_DIR}/external/yyjson"
)

# Yoga — flex-раскладка UI (Meta, MIT). Компилируем исходники напрямую (external/yoga/yoga/*.cpp),
# include-корень external/yoga (#include <yoga/...>). Требует C++20. Линкует только Engine (PRIVATE):
# Yoga-типы прячет UI_Yoga (pimpl), в игру не течёт — как SDL_ttf в FontManager.
file(GLOB_RECURSE YOGA_SRC CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/external/yoga/yoga/*.cpp")
add_library(yoga STATIC ${YOGA_SRC})
target_include_directories(yoga PUBLIC "${CMAKE_SOURCE_DIR}/external/yoga")
target_compile_features(yoga PUBLIC cxx_std_20)

# ============================================================
#  EngineEcs — ECS-ядро: ObjectManager + компоненты + сцены.
#  Лист: не зависит ни от рендера, ни от GPU. Линкуется и Engine, и Physics.
# ============================================================
add_library(EngineEcs STATIC)
target_compile_features(EngineEcs PUBLIC cxx_std_23)

set(ENGINEECS_SRC
    src/ObjectManager.cpp
    src/BaseComponents.cpp
    src/ComponentSerializer.cpp
)
set(ENGINEECS_HDR
    include/ObjectManager.h
    include/ObjectManager.inl
    include/BaseComponents.h
    include/SceneData.h
    include/Aliases.h
    include/ComponentSerializer.h
)
list(TRANSFORM ENGINEECS_SRC PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
list(TRANSFORM ENGINEECS_HDR PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
target_sources(EngineEcs PRIVATE ${ENGINEECS_SRC} ${ENGINEECS_HDR})
source_group("Source"  FILES ${ENGINEECS_SRC})
source_group("Headers" FILES ${ENGINEECS_HDR})

target_include_directories(EngineEcs PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_include_directories(EngineEcs PRIVATE
    "${CMAKE_SOURCE_DIR}/external/glm"                      # нужно для PCH.h
    "${CMAKE_SOURCE_DIR}/external/SDL3_shadercross/include" # тоже только ради общего PCH (косметика)
)
target_link_libraries(EngineEcs PUBLIC
    SDL3::SDL3                                              # BaseComponents.h включает SDL; цель даёт и include-пути
    yyjson                                                  # scene.json: колоночная сериализация ECS (ComponentSerializer.h раскрывает yyjson-типы)
)
target_precompile_headers(EngineEcs PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/PCH.h
)

# ============================================================
#  EngineGpu — GPU-кор: буферы, шейдеры, текстуры, пассы.
#  НЕ зависит от рендера/модели/материалов/ECS/ImGui/SDL_image.
#  Это та либа, которую будет линковать модуль физики.
# ============================================================
add_library(EngineGpu STATIC)
target_compile_features(EngineGpu PUBLIC cxx_std_23)

# ENGINE_PROFILE: кадровый профайлер (Prof::/PROF_SCOPE). ON (дев) — замеры активны;
# OFF (шиппинг) — FrameProfiler::Add/Frame пустеют inline, PROF_SCOPE исчезает, вся
# стоимость (мьютекс/хэш/строки/печать) уходит. PUBLIC: определение доходит и до
# Engine (линкует EngineGpu PUBLIC), где Prof:: в Engine.cpp/ThreadController.cpp.
option(ENGINE_PROFILE "Кадровый профайлер (Prof::/PROF_SCOPE)" ON)
target_compile_definitions(EngineGpu PUBLIC ENGINE_PROFILE=$<BOOL:${ENGINE_PROFILE}>)

set(ENGINEGPU_SRC
    src/TransferManager.cpp
    src/BufferManager.cpp
    src/BufferManager_Binds.cpp
    src/BufferManager_Update.cpp
    src/BufferManager_Utils.cpp
    src/ShaderManager.cpp
    src/ShaderManager_ShadersCreate.cpp
    src/ShaderManager_SPVLoad.cpp
    src/TextureManager.cpp
    src/PreviewPacker.cpp
    src/RenderManager.cpp
    src/RenderCommandData.cpp
    src/GpuTaskContext.cpp
    src/EngineProfiler.cpp
)
set(ENGINEGPU_HDR
    include/TransferManager.h
    include/BufferManager.h
    include/BufferData.h
    include/BufferUpdateStruct.h
    include/ShaderManager.h
    include/ShaderData.h
    include/ShaderTypes.h
    include/TextureManager.h
    include/TextureData.h
    include/PreviewPacker.h
    include/RenderManager.h
    include/RenderCommandData.h
    include/GpuTaskContext.h
    include/EngineProfiler.h
)
list(TRANSFORM ENGINEGPU_SRC PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
list(TRANSFORM ENGINEGPU_HDR PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
# .h добавляем в target_sources, чтобы они отображались в Solution Explorer (не компилируются).
target_sources(EngineGpu PRIVATE ${ENGINEGPU_SRC} ${ENGINEGPU_HDR})
source_group("Source"  FILES ${ENGINEGPU_SRC})
source_group("Headers" FILES ${ENGINEGPU_HDR})

# Публичные заголовки движка лежат в include/ — отдаём их потребителям.
# SDL3 и glm экспонируются PUBLIC, т.к. публичные заголовки GPU-кора их включают
# (раньше SDL3-инклуд протекал к игре случайно через ImGui — теперь явно отсюда).
target_include_directories(EngineGpu PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    "${CMAKE_SOURCE_DIR}/external/glm"
    "${CMAKE_SOURCE_DIR}/external/SDL3_shadercross/include"   # ShaderManager.h раскрывает SDL_ShaderCross_*
)
target_include_directories(EngineGpu PRIVATE
    "${CMAKE_SOURCE_DIR}/external/rectpack2D"                 # TextureManager: упаковка атласа
)

target_link_libraries(EngineGpu PUBLIC
    SDL3::SDL3          # SDL3-инклуды публичных заголовков GPU-кора приходят вместе с целью
)
target_link_libraries(EngineGpu PRIVATE
    "${CMAKE_SOURCE_DIR}/external/SDL3_shadercross/lib/SDL3_shadercross.lib"  # компиляция шейдеров — только внутри
)

target_precompile_headers(EngineGpu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/PCH.h
)

# ============================================================
#  Engine — рендер/оркестрация: пассы, data-модули, ECS, UI, цикл.
#  Линкует EngineGpu (PUBLIC) и добавляет всё остальное.
# ============================================================
add_library(Engine STATIC)
target_compile_features(Engine PUBLIC cxx_std_23)

# A/B-стенд префасса (Engine_PrepassBench.cpp) — компилируется как пустой TU, пока
# ENGINE_BENCH=OFF. Включить сравнение: cmake -DENGINE_BENCH=ON. Тело стенда под
# #if ENGINE_BENCH, боевой Engine.cpp его не тянет.
option(ENGINE_BENCH "Собирать A/B-стенд замеров префасса" OFF)

set(ENGINE_MANAGERS_SRC
    src/CameraManager.cpp
    src/MaterialManager.cpp
    src/ModelManager.cpp
    src/PipeManager.cpp
    src/ThreadController.cpp
    src/SlotController.cpp
    src/InputManager.cpp
    src/FontManager.cpp
)
set(ENGINE_MANAGERS_HDR
    include/CameraManager.h
    include/Camera.h
    include/MaterialManager.h
    include/ModelManager.h
    include/PipeManager.h
    include/ThreadController.h
    include/SlotController.h
    include/InputManager.h
    include/InputCommands.h
    include/TextureLoader.h
    include/FontManager.h
)
set(ENGINE_STRUCTS_SRC
    src/CameraStruct.cpp
    src/MaterialParamsSpec.cpp
)
set(ENGINE_STRUCTS_HDR
    include/CameraStruct.h
    include/MaterialData.h
    include/MaterialParams.h
    include/MaterialParamsSpec.h
    include/ModelData.h
    include/PositionStructure.h
    include/LightStruct.h
)
set(ENGINE_DATAMODULES_SRC
    src/BoundSphereDataModule.cpp
    src/IndirectDataModule.cpp
    src/LightDataModule.cpp
    src/PIB_DataModule.cpp
    src/TransformDataModule.cpp
    src/InstanceDataModule.cpp
    src/UI_DataModule.cpp
)
set(ENGINE_DATAMODULES_HDR
    include/BoundSphereDataModule.h
    include/IndirectDataModule.h
    include/LightDataModule.h
    include/PIB_DataModule.h
    include/TransformDataModule.h
    include/InstanceDataModule.h
    include/UI_DataModule.h
)
set(ENGINE_PRESETS_HDR
    include/TextureSamplerPresets.h
    include/TexturesPresets.h
)
set(ENGINE_UTILS_SRC
    src/BatchBuilder.cpp
    src/FPSCounter.cpp
    src/Utils.cpp
)
set(ENGINE_UTILS_HDR
    include/BatchBuilder.h
    include/FPSCounter.h
    include/Utils.h
    include/config.h
)
set(ENGINE_CORE_SRC
    src/Engine.cpp
    src/Engine_Frame.cpp
    src/Engine_Scene.cpp
    src/Engine_PrepassBench.cpp
    src/DefaultRenderPassSet.cpp
    src/DefaultUpdateSet.cpp
    src/DefaultCommandSet.cpp
    src/UI_ImGui.cpp
    src/UI_Widgets.cpp
    src/UI_Hierarchy.cpp
    src/UI_AssetBrowser.cpp
    src/UI_Inspector.cpp
    src/UI_ComponentEditor.cpp
    src/PCH.cpp
    src/EngineContext.cpp
    src/TextureLoader.cpp
    src/UI_Yoga.cpp
)
set(ENGINE_CORE_HDR
    include/Engine.h
    include/DefaultRenderPassSet.h
    include/DefaultUpdateSet.h
    include/DefaultCommandSet.h
    include/UI_ImGui.h
    include/UI_Internal.h
    include/UI_Widgets.h
    include/UI_ComponentEditor.h
    include/PCH.h
    include/PCH_Engine.h
    include/EngineContext.h
    include/UI_Yoga.h
)

# Абсолютим всё одинаково — иначе source_group не совпадёт с target_sources.
list(TRANSFORM ENGINE_MANAGERS_SRC    PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
list(TRANSFORM ENGINE_MANAGERS_HDR    PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
list(TRANSFORM ENGINE_STRUCTS_SRC     PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
list(TRANSFORM ENGINE_STRUCTS_HDR     PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
list(TRANSFORM ENGINE_DATAMODULES_SRC PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
list(TRANSFORM ENGINE_DATAMODULES_HDR PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
list(TRANSFORM ENGINE_PRESETS_HDR     PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
list(TRANSFORM ENGINE_UTILS_SRC       PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
list(TRANSFORM ENGINE_UTILS_HDR       PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
list(TRANSFORM ENGINE_CORE_SRC        PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
list(TRANSFORM ENGINE_CORE_HDR        PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)

target_sources(Engine PRIVATE
    ${ENGINE_MANAGERS_SRC}    ${ENGINE_MANAGERS_HDR}
    ${ENGINE_STRUCTS_SRC}     ${ENGINE_STRUCTS_HDR}
    ${ENGINE_DATAMODULES_SRC} ${ENGINE_DATAMODULES_HDR}
    ${ENGINE_PRESETS_HDR}
    ${ENGINE_UTILS_SRC}       ${ENGINE_UTILS_HDR}
    ${ENGINE_CORE_SRC}        ${ENGINE_CORE_HDR}
)

source_group("Source/Managers"     FILES ${ENGINE_MANAGERS_SRC})
source_group("Source/Structs"      FILES ${ENGINE_STRUCTS_SRC})
source_group("Source/DataModules"  FILES ${ENGINE_DATAMODULES_SRC})
source_group("Source/Utils"        FILES ${ENGINE_UTILS_SRC})
source_group("Source/Core"         FILES ${ENGINE_CORE_SRC})
source_group("Headers/Managers"    FILES ${ENGINE_MANAGERS_HDR})
source_group("Headers/Structs"     FILES ${ENGINE_STRUCTS_HDR})
source_group("Headers/DataModules" FILES ${ENGINE_DATAMODULES_HDR})
source_group("Headers/Presets"     FILES ${ENGINE_PRESETS_HDR})
source_group("Headers/Utils"       FILES ${ENGINE_UTILS_HDR})
source_group("Headers/Core"        FILES ${ENGINE_CORE_HDR})

target_include_directories(Engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_include_directories(Engine PRIVATE
    "${CMAKE_SOURCE_DIR}/external/SDL3_image/include"   # только TextureLoader.cpp
    "${CMAKE_SOURCE_DIR}/external/SDL3_ttf/include"     # только FontManager.cpp (растеризация глифов)
    "${CMAKE_SOURCE_DIR}/external/yoga"                 # только UI_Yoga.cpp (#include <yoga/...>)
)

# Свой PCH верхнего слоя: PCH.h + imgui.h (~8 TU таргета зовут ImGui). EngineGpu/EngineEcs
# остаются на чистом PCH.h — imgui в GPU/ECS-слои не протекает.
target_precompile_headers(Engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/PCH_Engine.h
)

target_link_libraries(Engine PUBLIC
    EngineGpu
    EngineEcs
)
target_link_libraries(Engine PRIVATE
    ImGui                                                            # только UI_*.cpp/Engine.cpp; игра ходит через фасад UI_ImGui

    "${CMAKE_SOURCE_DIR}/external/SDL3_image/lib/x64/SDL3_image.lib"  # только TextureLoader
    "${CMAKE_SOURCE_DIR}/external/SDL3_ttf/lib/x64/SDL3_ttf.lib"      # только FontManager (SDL_ttf)
    yyjson                                                            # файлы сцены/ресурсов (Save/LoadScene)
    yoga                                                              # только UI_Yoga (flex-раскладка; pimpl прячет типы)
)

target_compile_definitions(Engine PRIVATE
    $<$<BOOL:${ENGINE_BENCH}>:ENGINE_BENCH=1>   # тело Engine_PrepassBench.cpp
)
