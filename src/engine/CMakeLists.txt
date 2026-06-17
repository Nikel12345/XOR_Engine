add_library(Engine STATIC)

target_compile_features(Engine PUBLIC cxx_std_23)

# Все исходники движка
set(MANAGERS_SRC
    src/BufferManager.cpp
    src/BufferManager_Binds.cpp
    src/BufferManager_Update.cpp
    src/BufferManager_Utils.cpp
    src/CameraManager.cpp
    src/MaterialManager.cpp
    src/ModelManager.cpp
    src/ObjectManager.cpp
    src/PipeManager.cpp
    src/ResourceManager.cpp
    src/RenderManager.cpp
    src/ShaderManager.cpp
    src/ShaderManager_ShadersCreate.cpp
    src/ShaderManager_SPVLoad.cpp
    src/TextureManager.cpp
    src/TextureLoader.cpp
    src/ThreadController.cpp
    src/SlotController.cpp
    src/InputManager.cpp
)
set(MANAGERS_HDR
    include/BufferManager.h
    include/CameraManager.h
    include/MaterialManager.h
    include/ModelManager.h
    include/ObjectManager.h
    include/ObjectManager.inl
    include/PipeManager.h
    include/ResourceManager.h
    include/RenderManager.h
    include/ShaderManager.h
    include/TextureManager.h
    include/TextureLoader.h
    include/ThreadController.h
    include/SlotController.h
    include/InputManager.h
)

set(STRUCTS_SRC
    src/CameraStruct.cpp
    src/RenderCommandData.cpp
    src/BaseComponents.cpp
)
set(STRUCTS_HDR
    include/BufferData.h
    include/BufferUpdateStruct.h
    include/CameraStruct.h
    include/MaterialData.h
    include/ModelData.h
    include/PositionStructure.h
    include/SceneData.h
    include/ShaderData.h
    include/TextureData.h
    include/RenderCommandData.h
    include/BaseComponents.h
    include/LightStruct.h
)

set(PRESETS_HDR
    include/TextureSamplerPresets.h
    include/TexturesPresets.h
)

set(DATAMODULES_SRC
    src/BoundSphereDataModule.cpp
    src/CountBufferDataModule.cpp
    src/IndirectDataModule.cpp
    src/LightDataModule.cpp
    src/PIB_DataModule.cpp
    src/TransformDataModule.cpp
)
set(DATAMODULES_HDR
    include/BoundSphereDataModule.h
    include/CountBufferDataModule.h
    include/IndirectDataModule.h
    include/LightDataModule.h
    include/PIB_DataModule.h
    include/TransformDataModule.h
)

set(UTILS_SRC
    src/BatchBuilder.cpp
    src/FPSCounter.cpp
    src/Utils.cpp
)
set(UTILS_HDR
    include/BatchBuilder.h
    include/FPSCounter.h
    include/Utils.h
    include/config.h
)

set(CORE_SRC
    src/Engine.cpp
    src/DefaultRenderPassSet.cpp
    src/DefaultUpdateSet.cpp
    src/UI_ImGui.cpp
    src/PCH.cpp
    src/EngineContext.cpp
    src/GpuTaskContext.cpp
)
set(CORE_HDR
    include/Engine.h
    include/DefaultRenderPassSet.h
    include/DefaultUpdateSet.h
    include/UI_ImGui.h
    include/PCH.h
    include/EngineContext.h
    include/GpuTaskContext.h
)

list(TRANSFORM MANAGERS_SRC PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
list(TRANSFORM MANAGERS_HDR PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
list(TRANSFORM STRUCTS_SRC  PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
list(TRANSFORM STRUCTS_HDR  PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
list(TRANSFORM PRESETS_HDR  PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)

# ============================================================
# target_sources — только .cpp как PRIVATE, .h тоже добавляем
# чтобы VS их видел (они не компилируются, просто отображаются)
# ============================================================

target_sources(Engine PRIVATE
    ${MANAGERS_SRC} ${MANAGERS_HDR}
    ${STRUCTS_SRC}  ${STRUCTS_HDR}
    ${DATAMODULES_SRC} ${DATAMODULES_HDR}
    ${UTILS_SRC}    ${UTILS_HDR}
    ${CORE_SRC}     ${CORE_HDR}
)

# ============================================================
# Группы в Solution Explorer
# ============================================================

source_group("Source/Managers"     FILES ${MANAGERS_SRC})
source_group("Source/Structs"      FILES ${STRUCTS_SRC})
source_group("Source/DataModules"  FILES ${DATAMODULES_SRC})
source_group("Source/Utils"        FILES ${UTILS_SRC})
source_group("Source/Core"         FILES ${CORE_SRC})

source_group("Headers/Managers"    FILES ${MANAGERS_HDR})
source_group("Headers/Structs"     FILES ${STRUCTS_HDR})
source_group("Headers/Presets"     FILES ${PRESETS_HDR})
source_group("Headers/DataModules" FILES ${DATAMODULES_HDR})
source_group("Headers/Utils"       FILES ${UTILS_HDR})
source_group("Headers/Core"        FILES ${CORE_HDR})

# ============================================================
# PCH
# ============================================================

target_precompile_headers(Engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/PCH.h
)


# ====================== ВСЁ ВНЕШНЕЕ ======================

target_include_directories(Engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src                    # заголовки движка доступны игре
)

target_include_directories(Engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include          # для заголовков движка
)

target_include_directories(Engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
   "${CMAKE_SOURCE_DIR}/external/glm"
   "${CMAKE_SOURCE_DIR}/external/SDL3_image/include"
)

target_include_directories(Engine PRIVATE
    "${CMAKE_SOURCE_DIR}/external/SDL3/include"
    "${CMAKE_SOURCE_DIR}/external/SPIRV/include"
    "${CMAKE_SOURCE_DIR}/external/rectpack2D"
    "${CMAKE_SOURCE_DIR}/external/ImGui/include"

)

# ImGui
add_library(ImGui STATIC
    "${CMAKE_SOURCE_DIR}/external/ImGui/src/imgui.cpp"
    "${CMAKE_SOURCE_DIR}/external/ImGui/src/imgui_draw.cpp"
    "${CMAKE_SOURCE_DIR}/external/ImGui/src/imgui_tables.cpp"
    "${CMAKE_SOURCE_DIR}/external/ImGui/src/imgui_widgets.cpp"
    "${CMAKE_SOURCE_DIR}/external/ImGui/src/imgui_impl_sdl3.cpp"
    "${CMAKE_SOURCE_DIR}/external/ImGui/src/imgui_impl_sdlgpu3.cpp"
)

target_include_directories(ImGui PUBLIC
    "${CMAKE_SOURCE_DIR}/external/ImGui/include"
    "${CMAKE_SOURCE_DIR}/external/SDL3/include"
)

target_link_libraries(ImGui PUBLIC
    "${CMAKE_SOURCE_DIR}/external/SDL3/lib/libSDL3.dll.a"
)

# SPIRV
add_library(SPIRV STATIC
    "${CMAKE_SOURCE_DIR}/external/SPIRV/spirv_reflect.cpp"
)
target_include_directories(SPIRV PUBLIC
    "${CMAKE_SOURCE_DIR}/external/SPIRV"
    
    "${CMAKE_SOURCE_DIR}/external/SPIRV/include/spirv/unified1"
)

target_include_directories(Engine PUBLIC
    "${CMAKE_SOURCE_DIR}/external/SDL3_shadercross/include"
)

# Подключаем всё к Engine как PUBLIC
target_link_libraries(Engine PUBLIC
    ImGui
    SPIRV
    "${CMAKE_SOURCE_DIR}/external/SDL3/lib/libSDL3.dll.a"
    "${CMAKE_SOURCE_DIR}/external/SDL3_image/lib/x64/SDL3_image.lib"
    "${CMAKE_SOURCE_DIR}/external/SDL3_shadercross/lib/SDL3_shadercross.lib"

)


