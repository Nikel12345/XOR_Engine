#pragma once
#include <cstdint>
#include <vector>
#include <array>
#include <memory>
#include <functional>
#include <SDL3/SDL_gpu.h>
// UVL_Block/VariantLayout лежат в слепке ЗНАЧЕНИЯМИ (рендер копирует их в пуш) — нужен полный тип.
#include "RenderCommandData.h"

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
        std::vector<UVL_Block> texture_uvl;             // копия для пуша uniform'а UVL
        // Адресация той же таблицы (значением, как и она сама): рендер читает только слепок.
        VariantLayout variant_layout;
        // Невладеющий указатель на живой Material::params (адрес стабилен — как в дереве).
        // Содержимое UI может править на лету; это осознанно (мгновенный отклик слайдеров).
        const std::vector<uint8_t>* params = nullptr;
        uint32_t indirect_command_index = 0;            // первая команда мультидроу, индекс ЛОКАЛЬНЫЙ для прохода
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
        uint32_t num_instances = 0;    // сумма инстансов прохода
        uint32_t num_commands = 0;     // сумма команд прохода (model-батчей)
    };

    // Immutable после сборки; слоты шарят через shared_ptr (пересборка — только при
    // изменении дерева, стамп слота — присваивание указателя, O(1)).
    struct BatchLayout {
        std::vector<PassDrawList> passes;   // индекс = RenderPassStep::ordinal (порядок ordered_passes)
        // Индирект-буфер раскладки: GPU-двойник её команд. Один на всю раскладку (регионы
        // проходов лежат в нём подряд), поэтому свойство раскладки, а не батча. Резолв на
        // сборке — в цикле отрисовки остаётся пер-кадровый _GetGPUBufferForFrame, без лукапа.
        BufferData* indirectBuffer = nullptr;
    };

    // ── Регионы out-буферов ──
    // Индирект и out_pib нарезаны на регион НА ПРОХОД: в регионе — блок на каждый ДРОУ прохода
    // за кадр, в блоке только его команды/записи. Сколько блоков — решает вызывающий
    // (AskRegions); обычно это число камер прохода (тень: L), но камера лишь обычная причина:
    // UI рисуется один раз без всякой камеры — блок один. Размер = сумма по проходам
    // blocks*commands, а не произведение сумм — блок не таскает команды чужого прохода.
    // Раскладка пасс-мажорная, в порядке ordinal:
    //
    //   индирект: [ проход0: blocks0 x commands0 ][ проход1: blocks1 x commands1 ]…
    //   out_pib:  [ проход0: blocks0 x pib0      ][ проход1: blocks1 x pib1      ]…
    //
    // Здесь только СЛОВАРЬ типов — общий для вычислителя и потребителей (IndirectDataModule,
    // пуши каллинга, дроу). Считает регионы ЕДИНСТВЕННАЯ функция —
    // DefaultRenderPassNamespace::AskRegions: она же владеет знанием «сколько камер у прохода»,
    // которого у слоя раскладки нет. Контракт баз: StoreIndirect пишет буфер в этом же
    // пасс-мажорном порядке.
    struct Region {
        uint32_t command_blocks_count = 0;   // блоков (дроу за кадр); 0 — региона нет, диспатч пуст
        uint32_t commands = 0;      // команд на камеру (= PassDrawList::num_commands)
        uint32_t pib = 0;           // PIB-записей на камеру (= PassDrawList::num_instances)
        uint32_t first_pib = 0;     // начало сегмента прохода во ВХОДНОМ PIB (диапазон каллинга)
        uint32_t cmd_base = 0;      // база региона в индиректе, в командах
        uint32_t pib_base = 0;      // база региона в out_pib, в записях
    };

    struct Regions {
        std::vector<Region> per_pass;   // индекс = ordinal прохода
        uint32_t total_commands = 0;    // сумма blocks*commands — размер индиректа в командах
        uint32_t total_pib = 0;         // сумма blocks*pib — размер out_pib в записях
    };
}
