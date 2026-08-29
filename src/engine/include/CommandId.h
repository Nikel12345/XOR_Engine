#pragma once
#include <cstdint>

// Идентификаторы интерфейс-команд. По каждому один раз регистрируется функтор
// (см. InputManager::RegisterCommand), а в очередь летит только id + маленький POD-payload.
// Сами payload-структуры — в InputCommands.h: транспорт их не знает (void*),
// словарь нагрузок нужен только продьюсерам (UI_*) и хендлерам (DefaultCommandSet).
//
// Enum вынесен из InputManager.h ОТДЕЛЬНО и без единого include: его называет FieldSpec::cmd
// (ComponentSerializer.h, слой EngineEcs) — схема поля объявляет, какой командой уходит правка.
// Что команда делает, схема не знает и знать не должна; сам InputManager в ECS не попадает.
enum class CommandId : uint32_t {
    None,         // «команды нет» — обычное поле схемы, правка пишется прямо в колонку

    DeleteEntity,
    HideEntity,   // payload: FieldEditCmd* — Draw.visible живой энтити (+ дельта в батчи)
    SetEntityModel,    // payload: FieldEditCmd* (str = имя модели) — Model.name живой энтити:
                       // пишет имя, доводит длину Material.names до числа сабмешей, QueueUpdate
    SetEntityMaterial, // payload: FieldEditCmd* (num = индекс сабмеша, str = имя материала) —
                       // один слот Material.names живой энтити + QueueUpdate
    SetTransform, // payload: SetTransformCmd* на куче (16-float матрица не лезет в указатель),
                  // освобождается функтором после применения (см. DefaultCommandSet)
    SaveScene,    // payload: SceneIOCmd* на куче (имя сцены + путь), освобождает функтор
    LoadScene,    // payload: SceneIOCmd* на куче; грузить сцену можно только в sim-потоке
    SetMaterialTexture, // payload: SetMaterialTextureCmd* на куче (материал+слот+текстура);
                        // sim-поток правит Material::textures[role] + взводит пересборку батчей
    UpsertTexture,      // payload: UpsertTextureCmd* на куче (имя+атлас+путь+old_name). Создать/заменить
                        // текстуру; если old_name != name — это переименование, старую снимаем
    DeleteTexture,      // payload: DeleteTextureCmd* — удалить текстуру (материалы по имени → dummy)
    CreateMaterial,       // payload: CreateMaterialCmd* (имя из UI). Новый материал с sp "Lit" + дефолты
    AddMaterialShader,    // payload: MaterialShaderCmd* — добавить sp материалу (+ дефолты НОВЫХ ролей)
    RemoveMaterialShader, // payload: MaterialShaderCmd* — убрать sp у материала
    AddMaterialTextureVariant,    // payload: MaterialVariantCmd* — дописать вариант в слот-роль
    RemoveMaterialTextureVariant, // payload: MaterialVariantCmd* — убрать вариант (0 нельзя: дефолт)
    SetEntityTextureVariant,      // payload: EntityTextureVariantCmd* — какой вариант показывает энтити
    RenameMaterial,       // payload: RenameMaterialCmd* — ре-кей материала в словаре + пересборка
    UpsertModel,          // payload: UpsertModelCmd* — создать/перезагрузить модель из файла (in-place)
    RebuildShaderPipeline,// payload: RebuildShaderPipelineCmd* — spd правится in-place, тут инвалидация
                          // кэша пайплайна sp + пересборка (пайплайн строится из spd)
    DeleteShader,         // payload: RebuildShaderPipelineCmd* (то же поле shader) — удалить sp:
                          // пайплайн в отложенное удаление + erase sp (шейдеры релизятся по refcount)
    RecreateShader,       // payload: RecreateShaderCmd* — ПЕРЕСОЗДАНИЕ sp (удалить по старому имени,
                          // создать по новому из путей vs/fs) готовым CreateShaderProgram; как Upsert
                          // текстуры/модели. Ссылки материалов по старому имени НЕ чиним → fallback
    SetShaderPass,        // payload: SetShaderPassCmd* — сменить проход sp (associated_render_pass),
                          // инвалидация пайплайна + пересборка (моментально, как spd-тумблеры)
    UpsertVertexShader,   // payload: UpsertVertexShaderCmd*   — создать/пересобрать VSD из формы
    UpsertFragmentShader, // payload: UpsertFragmentShaderCmd* — создать/пересобрать FSD из формы
    UpsertComputeShader,  // payload: UpsertComputeShaderCmd*  — создать/пересобрать CSD из формы
    DeleteVertexShader,   // payload: ShaderDataNameCmd* — удалить VSD из реестра (sp по имени → fallback)
    DeleteFragmentShader, // payload: ShaderDataNameCmd*
    DeleteComputeShader,  // payload: ShaderDataNameCmd*
    CreateEntity,         // payload: CreateEntityCmd* — json одной сущности из staging-формы;
                          // sim: ObjectManager::LoadScene + пересборка батчей
    NudgeUINode,          // payload: UINodeNudgeCmd* — += смещение узла UI_Yoga (offset px XY + z-bias);
                          // sim: ui_yoga->NudgeNode + MarkDirty (гизмо XY / кнопки Z в инспекторе)

    COUNT
};
