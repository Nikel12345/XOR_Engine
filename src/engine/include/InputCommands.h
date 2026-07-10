#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "ShaderTypes.h"  // spd/VertexSemantic/TextureSlotRole в payload'ах шейдер-команд
#include "Aliases.h"      // BufferDataName в RecreateShaderCmd

// Payload-структуры интерфейс-команд (CommandId + транспорт — InputManager.h). Отделены от
// очереди: InputManager — механизм доставки и полезных нагрузок не знает; их словарь — здесь,
// рядом с редакторными хендлерами (DefaultCommandSet) и продьюсерами (UI_*). Конвенция времени
// жизни: строки не пакуются в указатель → продьюсер (UI) выделяет структуру на куче,
// консьюмер-функтор (sim-поток) применяет и удаляет.

// Применить изменённый spd шейдера: sp->spd уже поправлен в UI in-place, здесь сбрасываем
// кэшированный пайплайн (строится из spd) и взводим его пересоздание + пересборку батчей.
struct RebuildShaderPipelineCmd { std::string shader; };

// Пересоздание sp по кнопке-подтверждению (как UpsertTexture/UpsertModel): удалить старую sp,
// создать новую под newName из путей vs/fs. Неизменяемые в форме части (буферы/слоты/пасс/spd/
// push-константы) переносятся со старой. Ссылки материалов по СТАРОМУ имени не патчим (конвенция:
// на пересборке → fallback; переименовывай до назначения). Пустой путь → прежний. Строки на куче.
struct RecreateShaderCmd {
    std::string oldName;
    std::string newName;
    std::string vsName;    // имя вершинного шейдера из реестра (пусто → прежний)
    std::string fsName;    // имя фрагментного шейдера из реестра (пусто → прежний)
    std::string passName;  // имя прохода (пусто → прежний). Вся правка sp — под одной кнопкой
    ShaderProgramDescription spd;   // состояние пайплайна (тоже под кнопкой, не мгновенно)
    // Storage-буферы стадий ПО ИМЕНИ (BufferDataName = каноничный ключ реестра, порядок = слоты
    // бинда). Указатели стабильны (имена — статические литералы DefaultBuffersNames), поэтому их
    // безопасно нести через очередь команд и хранить в sp; резолв в BufferData* — на сборке батча.
    std::vector<BufferDataName> vsBuffers;
    std::vector<BufferDataName> fsBuffers;
    // Слот-роли текстур (взаимоисключающие). Дубли отсеет CreateShaderProgram, но UI и так их не даёт.
    std::vector<TextureSlotRole> slots;
};

// Смена прохода sp: имя прохода резолвится в RenderPassStep* (PassManager) и кладётся в
// sp->associated_render_pass; пайплайн зависит от форматов прохода → инвалидация + пересборка.
struct SetShaderPassCmd {
    std::string shader;
    std::string pass;
};

// Upsert шейдер-данных из формы редактора SD (как UpsertTexture/UpsertModel): компиляция по имени,
// oldName != name → удалить старую запись. Пайплайны ссылающихся sp/csp инвалидируются в хендлере.
// pull у вершинника — набор+ПОРЯДОК семантик из FMT_PosUVNormal (пока без реестра раскладок).
struct UpsertVertexShaderCmd   { std::string name, path, oldName; std::vector<ShaderBase::VertexSemantic> pull; };
struct UpsertFragmentShaderCmd { std::string name, path, oldName; };
struct UpsertComputeShaderCmd  { std::string name, path, oldName; };
struct ShaderDataNameCmd       { std::string name; };   // удаление vs/fs/cs (тип — по CommandId)

// Создание/замена модели из файла (аналог UpsertTexture). Существующую перезагружает В ТОТ ЖЕ
// объект (указатель у энтити жив; старая геометрия в буфере остаётся — reclaim'а нет). Процедурные
// модели (сгенерированы кодом) редактор не трогает: у них пустые пути. Строки на куче, функтор удалит.
struct UpsertModelCmd {
    std::string name;
    std::string model_path;
    std::string index_path;
    uint32_t    anchor = 0;   // AnchorShift как uint32_t (без завязки заголовка на ModelData.h)
};

// Имя нового материала считает UI (свободное material_N) — чтобы сразу выбрать созданный.
struct CreateMaterialCmd { std::string name; };

// Переименование материала (delete+create на уровне словаря). Валидность (непусто/иное/свободно)
// проверяет UI (галочка активна только тогда); менеджер повторно защищается сам.
struct RenameMaterialCmd { std::string oldName; std::string newName; };

// Добавить/убрать sp у материала. Слоты диктует объединение required_slots его sp; роль ШАРИТСЯ
// между sp (одна карта material->textures). Строки на куче, функтор удалит.
struct MaterialShaderCmd {
    std::string material;
    std::string shader;
};

// Создание/замена текстуры из формы редактора. Один путь и для «создать», и для «редактировать»:
// удаляем хэндл с этим именем (no-op, если нет) и грузим заново из файла в атлас. Материалы держат
// текстуру по имени → сами перепривяжутся на пересборке. Строки на куче, функтор удалит.
struct UpsertTextureCmd {
    std::string name;
    std::string atlas;
    std::string path;
    uint32_t    conv = 0;    // ChannelConvention как uint32_t (без завязки заголовка на TextureData.h)
    std::string old_name;    // ранее выбранная текстура; != name → переименование (старую снять)
};

// Удаление текстуры. Материалы, ссылавшиеся на неё по имени, на пересборке дадут dummy.
struct DeleteTextureCmd { std::string name; };

// Смена текстуры слота материала. Материал ссылается на текстуру ПО ИМЕНИ (name-based), поэтому
// правка = замена строки в Material::textures[role]. Обязателен ребилд батчей: в батче лежит уже
// РАЗРЕШЁННЫЙ UVL текстуры, его надо пересчитать. Строки в указатель не паковать → куча, функтор удалит.
struct SetMaterialTextureCmd {
    std::string material;
    uint32_t    role;      // TextureSlotRole как uint32_t (без завязки на порядок объявлений)
    std::string texture;
};

// Нагрузка save/load: строки в указатель не упаковать, поэтому продьюсер (UI) выделяет
// на куче, а консьюмер (sim-поток) применяет и удаляет. Save/Load — операции над ECS и
// батчами, обязаны идти в sim-потоке, а не из render-потока, где живёт UI.
struct SceneIOCmd {
    std::string scene;
    std::string path;
};

// Полезная нагрузка SetTransform. В отличие от Delete/Hide данные не упаковать в
// указатель (16 float), поэтому продьюсер (гизмо в UI) выделяет это на куче, а
// консьюмер (sim-поток) применяет и сам удаляет. matrix — мировой трансформ в
// column-major раскладке glm (то, что отдаёт/принимает ImGuizmo).
struct SetTransformCmd {
    uint32_t entity;
    float    matrix[16];
};
