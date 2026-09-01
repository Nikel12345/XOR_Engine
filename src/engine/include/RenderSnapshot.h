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
        uint32_t indirect_command_index = 0;            // первая команда мультидроу, индекс ЛОКАЛЬНЫЙ для группы
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

    // Групп камер (и, значит, регионов) в раскладке. Больше — только если появится третий тип
    // камер: тогда это число и прогон под него.
    inline constexpr uint32_t kMaxCameraGroups = 4;

    // Прогон проходов, делящих ОДИН регион команд. О камерах здесь ни слова: сколько камер у
    // региона — знает только тот, кто их заводит (набор проходов), и передаёт это в BuildRegions
    // отдельно. Прогоны обязаны быть непрерывными и не пересекаться; непокрытый проход попадает
    // в группу 0 (исторический дефолт — камера игрока), см. GroupOfPass.
    struct GroupSpan {
        uint32_t first_pass = 0;
        uint32_t pass_count = 0;
    };

    inline uint32_t GroupOfPass(const std::array<GroupSpan, kMaxCameraGroups>& spans, uint32_t pass)
    {
        for (uint32_t g = 0; g < kMaxCameraGroups; ++g)
            if (pass >= spans[g].first_pass && pass < spans[g].first_pass + spans[g].pass_count)
                return g;
        return 0;
    }

    // Immutable после сборки; слоты шарят через shared_ptr (пересборка — только при
    // изменении дерева, стамп слота — присваивание указателя, O(1)).
    struct BatchLayout {
        std::vector<PassDrawList> passes;   // индекс = RenderPassStep::ordinal (порядок ordered_passes)
        // Прогоны, ПО КОТОРЫМ пронумерованы команды этой раскладки. Лежат здесь, а не считаются
        // заново каждым читателем: нумерация и регионы обязаны быть из одного источника, иначе
        // разъедутся молча. Кладёт FinalizeOffsets.
        std::array<GroupSpan, kMaxCameraGroups> groups{};
        // Индирект-буфер раскладки: GPU-двойник её команд. Один на всю раскладку (регионы
        // групп лежат в нём подряд), поэтому свойство раскладки, а не прохода/батча. Резолв на
        // сборке — в цикле отрисовки остаётся пер-кадровый _GetGPUBufferForFrame, без лукапа.
        BufferData* indirectBuffer = nullptr;
    };

    // ── Регионы out-буферов ──
    // Индирект и out_pib нарезаны НЕ на (камера × всё), а на регион на группу камер: в регионе
    // группы лежат только её команды/записи, по блоку на камеру. Размер = сумма по группам
    // cams*commands, а не произведение сумм — камера не таскает команды чужого прохода.
    //
    //   индирект: [ группа0: cams0 блоков по commands0 ][ группа1: cams1 блоков по commands1 ]…
    //   out_pib:  [ группа0: cams0 блоков по pib0      ][ группа1: cams1 блоков по pib1      ]…
    //
    // Порядок регионов = порядок group id, и задан он ЗДЕСЬ (единственное место). Группа 0
    // (камера игрока) поэтому всегда с базой 0 — проходы игрока рисуют со смещением 0, как и до
    // регионов. Индекс команды в TextureDraw и значение EntityToCmd — локальные для ГРУППЫ
    // (не для прохода: в группе игрока проходов несколько, их команды делят один регион).
    struct Region {
        uint32_t cams = 0;          // камер в группе (0 — региона нет)
        uint32_t commands = 0;      // команд на камеру (сумма по проходам группы)
        uint32_t pib = 0;           // PIB-записей на камеру (сумма по проходам группы)
        uint32_t first_pib = 0;     // начало сегмента группы во ВХОДНОМ PIB (для caller'а каллинга)
        uint32_t cmd_base = 0;      // база региона в индиректе, в командах
        uint32_t pib_base = 0;      // база региона в out_pib, в записях
    };

    struct Regions {
        std::array<Region, kMaxCameraGroups> g{};
        uint32_t total_commands = 0;   // сумма cams*commands — размер индиректа в командах
        uint32_t total_pib = 0;        // сумма cams*pib — размер out_pib в записях
    };

    // cams — число камер каждой группы (у вызывающего: {1, L, …}); слой раскладки о камерах
    // ничего не знает, они приходят из LightDataModule.
    inline Regions BuildRegions(const BatchLayout* layout,
                                const std::array<uint32_t, kMaxCameraGroups>& cams)
    {
        Regions r{};
        for (uint32_t i = 0; i < kMaxCameraGroups; ++i) r.g[i].cams = cams[i];

        if (layout) {
            uint32_t prev_group = ~0u;
            for (uint32_t i = 0; i < layout->passes.size(); ++i) {
                const PassDrawList& p = layout->passes[i];
                // Прогоны берём из САМОЙ раскладки: по ним же пронумерованы её команды.
                const uint32_t gid = GroupOfPass(layout->groups, i);
                if (gid != prev_group) {
                    // Первый проход прогона задаёт начало PIB-сегмента группы.
                    r.g[gid].first_pib = p.first_instance;
                    prev_group = gid;
                }
                r.g[gid].commands += p.num_commands;
                r.g[gid].pib      += p.num_instances;
            }
        }

        uint32_t cmd_base = 0, pib_base = 0;
        for (uint32_t i = 0; i < kMaxCameraGroups; ++i) {
            r.g[i].cmd_base = cmd_base;   cmd_base += r.g[i].cams * r.g[i].commands;
            r.g[i].pib_base = pib_base;   pib_base += r.g[i].cams * r.g[i].pib;
        }
        r.total_commands = cmd_base;
        r.total_pib = pib_base;
        return r;
    }
}
