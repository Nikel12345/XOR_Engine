#pragma once
#include <cstdint>
#include <vector>
#include <memory>
#include <functional>
#include <SDL3/SDL_gpu.h>
#include "TextureData.h"

struct BufferData;
struct PushConstantBinder;

// Пер-слотовые CPU-слепки «что рисуем», которые sim готовит для рендер-потока ВМЕСТЕ с
// GPU-буферами слота (та же фаза prepare, тот же слот). Рендер читает только их через
// Ask*(slot) — живые ECS/дерево батчей ему запрещены. Синхронизация не нужна: sim пишет
// слепок, пока владеет слотом (RESERVED), рендер читает после PREPARED — happens-before
// даёт жизненный цикл слота (SlotController), ровно как у GPU-буферов.
// Типы — чистые данные без логики; владельцы массивов [BUFFERING_LEVEL] — сами модули.
namespace RenderSnap {

    // Одна теневая камера = один слой теневого атласа. Порядок в cams — порядок записи
    // LIGHT_CAMERA_BUFFER (spot → sphere×6 → direct-каскады): индекс = camera_index,
    // совпадение с буфером гарантировано тем, что слепок пишет тот же модуль в тот же prepare.
    struct ShadowCam {
        float   max_range = 0.0f;    // far камеры (spot/sphere: GetMaxDistance, direct: CascadeFar)
        uint8_t is_ortho = 0;        // 1 — directional (ortho), см. ShadowPushData
        uint8_t needs_render = 0;    // светокомпонентный needsUpdate на момент prepare
    };

    struct LightCams {
        std::vector<ShadowCam> cams;
    };

    // ── Плоская раскладка дерева батчей (двойник indirect_buffer[slot]) ──
    // Иерархия зеркалит дерево (shader → atlas → texture), но значениями: рендер записывает
    // команды слота ровно по той раскладке, по которой prepare залил его индирект.

    struct TextureDraw {
        std::vector<TextureData> texture_uvl;           // копия для пуша uniform'а UVL
        // Невладеющий указатель на живой Material::params (адрес стабилен — как в дереве).
        // Содержимое UI может править на лету; это осознанно (мгновенный отклик слайдеров).
        const std::vector<uint8_t>* params = nullptr;
        uint32_t indirect_command_index = 0;            // первая команда мультидроу в блоке камеры
        uint32_t draw_count = 0;                        // число команд (= model_batches)
    };

    struct AtlasGroup {
        std::vector<SDL_GPUTextureSamplerBinding> texture_binding;
        std::vector<TextureDraw> draws;
    };

    struct ShaderGroup {
        SDL_GPUGraphicsPipeline* pipeline = nullptr;
        std::function<void(const PushConstantBinder&, const void*)> push_func;
        // Вершинные СТРИМЫ пула из объявления vs (порядок = слоты пайплайна). Пустой список =
        // резолв сорвался → draw пропускается (бинд не того стрима в слот = UB, не деградация).
        std::vector<BufferData*> vertexBuffers;
        // Индексный буфер пула, которому принадлежат стримы (у одного пула — один; резолв на
        // сборке батча). nullptr = резолв сорвался → draw пропускается, как у стримов.
        BufferData* indexBuffer = nullptr;
        std::vector<BufferData*> vertexStorageBuffers;
        std::vector<BufferData*> fragmentStorageBuffers;
        uint32_t frag_uniform_count = 0;                // гейт пуша params (как в дереве)
        std::vector<AtlasGroup> atlases;
    };

    struct PassDrawList {
        std::vector<ShaderGroup> shaders;
        // Глобальные сэмплеры прохода (тень, env-куб), УЖЕ отрезолвленные в SDL-биндинги на
        // сборке батча. В самом RenderPassStep они лежат как TextureAtlas* (стабильные
        // указатели): GPU-текстуру атласа могут пересоздать, и держать её копию с момента
        // setup нельзя — протухнет. Резолв здесь, а не в цикле отрисовки: значение постоянно
        // по всем шейдер-батчам прохода, а рендер обязан читать только слепок.
        std::vector<SDL_GPUTextureSamplerBinding> global_texture_bindings;
        uint32_t first_instance = 0;   // PIB-офсет начала прохода (сквозная нумерация FinalizeOffsets)
        uint32_t num_instances = 0;    // сумма инстансов прохода (SHADOW_PASS → shadow_pib каллинга)
    };

    // Immutable после сборки; слоты шарят через shared_ptr (пересборка — только при
    // изменении дерева, стамп слота — присваивание указателя, O(1)).
    struct BatchLayout {
        std::vector<PassDrawList> passes;   // индекс = RenderPassStep::ordinal (порядок ordered_passes)
        // Индирект-буфер раскладки: GPU-двойник её команд (нумерация сквозная через все проходы,
        // поэтому свойство ВСЕЙ раскладки, не прохода/батча). Резолв на сборке — в цикле
        // отрисовки остаётся только пер-кадровый _GetGPUBufferForFrame, без лукапа по имени.
        BufferData* indirectBuffer = nullptr;
        uint32_t num_commands = 0;          // всего индирект-команд на камеру
        uint32_t num_instances = 0;         // всего PIB-записей (сумма инстансов всех батчей)
    };
}
