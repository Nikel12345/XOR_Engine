#pragma once
#include <cstdint>

// Лист без зависимостей: его видит EngineEcs (FieldSpec::cmd), а нагрузки команд — уровнем выше,
// в InputCommands.h. Слить их в один заголовок значит утянуть туда же весь словарь структур.
enum class CommandId : uint32_t {
    None,

    DeleteEntity,
    HideEntity,
    SetEntityModel,
    SetEntityMaterial,
    SetTransform,
    SaveScene,
    LoadScene,
    SetMaterialTexture,
    UpsertTexture,
    DeleteTexture,
    CreateMaterial,
    AddMaterialShader,
    RemoveMaterialShader,
    AddMaterialTextureVariant,
    RemoveMaterialTextureVariant,
    SetEntityTextureVariant,
    RenameMaterial,
    UpsertModel,
    DeleteShader,
    RecreateShader,
    UpsertVertexShader,
    UpsertFragmentShader,
    UpsertComputeShader,
    DeleteVertexShader,
    DeleteFragmentShader,
    DeleteComputeShader,
    CreateEntity,
    NudgeUINode,

    COUNT
};
