#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <utility>
#include "ShaderTypes.h"
#include "Aliases.h"
#include "InputManager.h"

// Пара «команда → её нагрузка»: очередь несёт CommandId и const void*, а какая структура чья —
// говорит специализация рядом с ней. Продьюсеры и хендлеры ходят через cmd::Push/cmd::Register,
// поэтому перепутанный тип не компилируется.
template<CommandId Id> struct CommandPayload;
template<CommandId Id> using CommandPayloadT = typename CommandPayload<Id>::type;

namespace cmd {

    // Очередь данные НЕ копирует: нагрузку выделяет продьюсер, освобождает Register по применении.
    template<CommandId Id, class... Args>
    void Push(InputManager* im, Args&&... args)
    {
        im->PushCommand(Id, new CommandPayloadT<Id>{ std::forward<Args>(args)... });
    }

    template<CommandId Id, class Fn>
    void Register(InputManager& im, Fn&& fn)
    {
        im.RegisterCommand(Id, [fn = std::forward<Fn>(fn)](EngineContext* ctx, const void* data)
        {
            const CommandPayloadT<Id>* payload = static_cast<const CommandPayloadT<Id>*>(data);
            fn(ctx, *payload);
            delete payload;
        });
    }

}

// Нагрузка полей схемы (FieldSpec::cmd). Её пушит generic-редактор компонентов РАНТАЙМНЫМ id —
// единственный путь мимо cmd::Push, то есть единственное место, где пару никто не проверит.
// Значение в колонку пишет хендлер, UI-поток её не трогает.
struct FieldEditCmd {
    uint32_t    entity;      // Entity как uint32_t — заголовок не завязан на BaseComponents.h
    std::string component;
    std::string field;
    double      num = 0.0;
    std::string str;
};
template<> struct CommandPayload<CommandId::HideEntity>         { using type = FieldEditCmd; };
template<> struct CommandPayload<CommandId::SetEntityModel>     { using type = FieldEditCmd; };
template<> struct CommandPayload<CommandId::SetEntityMaterial>  { using type = FieldEditCmd; };

struct ShaderProgramNameCmd { std::string shader; };
template<> struct CommandPayload<CommandId::DeleteShader> { using type = ShaderProgramNameCmd; };

// Правка sp = ПЕРЕСОЗДАНИЕ под одной кнопкой: пустое поле означает «оставить прежнее», а ссылки
// материалов по старому имени не чинятся — на пересборке они дадут fallback.
struct RecreateShaderCmd {
    std::string oldName;
    std::string newName;
    std::string vsName;
    std::string fsName;
    std::string passName;
    ShaderProgramDescription spd;
    std::vector<BufferDataName> vsBuffers;   // порядок = слоты бинда
    std::vector<BufferDataName> fsBuffers;
    std::vector<TextureSlotRole> slots;
};
template<> struct CommandPayload<CommandId::RecreateShader> { using type = RecreateShaderCmd; };

// defines едут вместе с путём: Upsert ПЕРЕСОЗДАЁТ шейдер-данные из формы, и набор, не доехавший до
// команды, был бы стёрт правкой имени — шейдер молча пересобрался бы на дефолтах #ifndef.
// pull — семантики вершинника; порядок в нём не значим, слоты задаёт таблица стримов пула.
struct UpsertVertexShaderCmd   { std::string name, path, oldName, pool; std::vector<ShaderBase::VertexSemantic> pull; ShaderDefines defines; };
template<> struct CommandPayload<CommandId::UpsertVertexShader> { using type = UpsertVertexShaderCmd; };
struct UpsertFragmentShaderCmd { std::string name, path, oldName; ShaderDefines defines; };
template<> struct CommandPayload<CommandId::UpsertFragmentShader> { using type = UpsertFragmentShaderCmd; };
struct UpsertComputeShaderCmd  { std::string name, path, oldName; ShaderDefines defines; };
template<> struct CommandPayload<CommandId::UpsertComputeShader> { using type = UpsertComputeShaderCmd; };
struct ShaderDataNameCmd       { std::string name; };
template<> struct CommandPayload<CommandId::DeleteVertexShader>   { using type = ShaderDataNameCmd; };
template<> struct CommandPayload<CommandId::DeleteFragmentShader> { using type = ShaderDataNameCmd; };
template<> struct CommandPayload<CommandId::DeleteComputeShader>  { using type = ShaderDataNameCmd; };

// Перезагрузка идёт В ТОТ ЖЕ объект (указатель у энтити жив), старая геометрия в буфере остаётся:
// reclaim'а нет.
struct UpsertModelCmd {
    std::string name;
    std::string model_path;
    std::string index_path;
    uint32_t    anchor = 0;   // AnchorShift как uint32_t (без завязки заголовка на ModelData.h)
    std::string old_name;     // ранее выбранная модель; != name → переименование ячейки реестра
};
template<> struct CommandPayload<CommandId::UpsertModel> { using type = UpsertModelCmd; };

struct CreateMaterialCmd { std::string name; };
template<> struct CommandPayload<CommandId::CreateMaterial> { using type = CreateMaterialCmd; };

struct RenameMaterialCmd { std::string oldName; std::string newName; };
template<> struct CommandPayload<CommandId::RenameMaterial> { using type = RenameMaterialCmd; };

// Слоты материала — объединение required_slots его sp, и текстура роли ШАРИТСЯ между ними:
// карта Material::textures одна на материал.
struct MaterialShaderCmd {
    std::string material;
    std::string shader;
};
template<> struct CommandPayload<CommandId::AddMaterialShader>    { using type = MaterialShaderCmd; };
template<> struct CommandPayload<CommandId::RemoveMaterialShader> { using type = MaterialShaderCmd; };

struct UpsertTextureCmd {
    std::string name;
    std::string atlas;
    std::string path;
    uint32_t    conv = 0;    // ChannelConvention как uint32_t (без завязки заголовка на TextureData.h)
    std::string old_name;    // ранее выбранная текстура; != name → переименование (старую снять)
    // Кубмапа-крест 4x3 вместо обычной текстуры. Приходит из формы, а НЕ выводится из типа атласа:
    // атлас выбирает тот же человек, и молча менять смысл его выбора команда не должна.
    bool        cube = false;
};
template<> struct CommandPayload<CommandId::UpsertTexture> { using type = UpsertTextureCmd; };

struct DeleteTextureCmd { std::string name; };
template<> struct CommandPayload<CommandId::DeleteTexture> { using type = DeleteTextureCmd; };

struct SetMaterialTextureCmd {
    std::string material;
    uint32_t    role;      // TextureSlotRole как uint32_t (без завязки на порядок объявлений)
    uint32_t    variant;   // 0 — дефолт, то, что рисуется без переключения
    std::string texture;
};
template<> struct CommandPayload<CommandId::SetMaterialTexture> { using type = SetMaterialTextureCmd; };

// Структурная правка: меняются длина таблицы UVL и нумерация ячеек секции, поэтому дерево батчей
// пересобирается — в отличие от смены ЗНАЧЕНИЯ варианта у сущности, которая его не трогает вовсе.
struct MaterialVariantCmd {
    std::string material;
    uint32_t    role;
    uint32_t    variant;   // у Add игнорируется (всегда в конец), у Remove — что убрать
};
template<> struct CommandPayload<CommandId::AddMaterialTextureVariant>    { using type = MaterialVariantCmd; };
template<> struct CommandPayload<CommandId::RemoveMaterialTextureVariant> { using type = MaterialVariantCmd; };

// Правка поля НА МЕСТЕ: архетип тот же, дерево батчей не трогается ни SetDirtyBatches, ни QueueUpdate.
struct EntityTextureVariantCmd {
    uint32_t entity;      // Entity как uint32_t — заголовок не завязан на BaseComponents.h
    uint32_t mat_index;   // какой материал сущности (= submesh.material_index)
    uint32_t role;
    uint32_t variant;     // 0 = дефолт; запись нуля УБИРАЕТ пару из states (список разреженный)
};
template<> struct CommandPayload<CommandId::SetEntityTextureVariant> { using type = EntityTextureVariantCmd; };

struct SceneIOCmd {
    std::string scene;
    std::string path;
};
template<> struct CommandPayload<CommandId::SaveScene> { using type = SceneIOCmd; };
template<> struct CommandPayload<CommandId::LoadScene> { using type = SceneIOCmd; };

// Сущность едет ТЕКСТОМ СЦЕНЫ: хендлер гонит её тем же ObjectManager::LoadScene, что и файл, —
// формат, дефолты и ремапы формы и файла совпадают по построению.
struct CreateEntityCmd {
    std::string scene;   // целевая сцена (активная на момент нажатия Create)
    std::string json;
};
template<> struct CommandPayload<CommandId::CreateEntity> { using type = CreateEntityCmd; };

struct SetTransformCmd {
    uint32_t entity;
    float    matrix[16];   // мировой трансформ в column-major раскладке glm (то, что даёт ImGuizmo)
};
template<> struct CommandPayload<CommandId::SetTransform> { using type = SetTransformCmd; };

// ПРИРАЩЕНИЕ, а не значение: XY в пикселях раскладки (offset применяется пост-layout), Z — bias слоя.
struct UINodeNudgeCmd {
    uint32_t node;
    float    ddx, ddy, ddz;
};
template<> struct CommandPayload<CommandId::NudgeUINode> { using type = UINodeNudgeCmd; };
