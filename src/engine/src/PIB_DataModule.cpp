#include "PCH.h"
#include "BaseComponents.h"
#include "PIB_DataModule.h"
#include "ObjectManager.h"
#include "BufferManager.h"
#include "RenderCommandData.h"
#include "RenderManager.h"
#include "EngineProfiler.h"

PIB_DataModule::PIB_DataModule()
{
    for (uint64_t& r : pib_last_revision) r = ~0ull;
    for (uint64_t& r : e2c_last_revision) r = ~0ull;
}

uint32_t PIB_DataModule::ComputeElementCount(PassManager* rm) const
{
    uint32_t count = 0;

    for (RenderPassStep* rp : rm->GetOrderedRenderPasses())
    {
        for (const auto& [_, sb] : rp->shader_batches)
        {
            for (const auto& [_, ab] : sb.atlases_batches)
            {
                for (const auto& [_, tb] : ab.texture_batches)
                {
                    for (const auto& [_, mb] : tb.model_batches) {
                        count += mb.instanceCount;
                    }
                };
            }
        }
    }

    return count;
}

uint32_t PIB_DataModule::CalculatePIBSizes(PassManager* rm, uint64_t revision, uint8_t slot)
{
    if (revision == pib_last_revision[slot]) return 0;

    total_elements = ComputeElementCount(rm);
    pib_last_revision[slot] = revision;

    return total_elements * sizeof(uint32_t);
}

// Тот же отбор и та же арифметика строки, что в RecalculateInstanceOffsets/TransformDataModule:
// строка = render_instance_base архетипа + индекс энтити в нём. Обход последовательный, поэтому
// заполнение 1М строк стоит один линейный проход вместо миллионов поисков в хэш-таблицах.
void PIB_DataModule::BuildRowTable(SceneData* scene)
{
    row_of.assign(scene->next_entity_id, kPibNoRow);

    for (auto& [sig, arch] : scene->archetypes) {
        if (!arch.get_array<DrawComponent>() || !arch.get_array<Positions>()) continue;

        const uint32_t base = arch.render_instance_base;
        const size_t   n    = arch.entities.size();
        for (size_t i = 0; i < n; ++i) {
            const Entity e = arch.entities[i];
            if (e < row_of.size()) row_of[e] = base + safe_u32(i);   // id вне таблицы = не из этой сцены
        }
    }
}

void PIB_DataModule::StorePIB(BufferManager* bm, PassManager* rm, UploadTask* task, ObjectManager* om)
{
    SceneData* scene = om->GetActiveScene();
    if (!scene) return;

    // Строки закэшированы в самих записях, поэтому обычная заливка их только читает.
    // Перезаписать ВСЕ нужно ровно тогда, когда изменился состав сущностей: тогда едут и
    // render_instance_base архетипов, и индексы внутри них, и старая строка не годится
    // НИКОМУ - в том числе тем, кого правка не касалась (swap_remove в архетипе переносит
    // чужую сущность на освободившийся индекс, и её строка меняется без всякой команды).
    const uint64_t rev = om->EntityRevision();
    const bool refresh = (rev != row_table_revision);
    if (refresh) {
        PROF_SCOPE(Sim, "     pib_row_table");
        BuildRowTable(scene);
        row_table_revision = rev;
    }

    // Пишем ПРЯМО в transfer-буфер: промежуточный вектор на total_elements — лишняя аллокация
    // и лишний memcpy на мегабайты. Размер задан size-фазой (CalculatePIBSizes) по ТОМУ ЖЕ
    // дереву, что обходим ниже, поэтому запросить его целиком корректно.
    uint32_t* dst = static_cast<uint32_t*>(
        bm->AcquireTransferWritePtr(task, total_elements * sizeof(uint32_t)));
    if (!dst) return;

    PROF_SCOPE(Sim, "     pib_gather");
    // Обход НЕконстантный: заполненная строка остаётся в записи и живёт до следующей смены
    // состава. Дерево приватно для sim-потока, на нём же исполняется и заливка; запись
    // идемпотентна, поэтому повтор по слотам буферизации безвреден.
    uint32_t n = 0;
    for (RenderPassStep* rp : rm->GetOrderedRenderPasses())
        for (auto& [_, sb] : rp->shader_batches)
            for (auto& [_, ab] : sb.atlases_batches)
                for (auto& [_, tb] : ab.texture_batches)
                    for (auto& [_, mb] : tb.model_batches) {
                        // Страховка от расхождения size-фазы и обхода (переполнить чужую память
                        // нельзя); проверка на батч, а не на запись — вне горячего цикла.
                        if (n + mb.pib_sub_buffer.size() > total_elements) continue;
                        for (uint64_t& rec : mb.pib_sub_buffer) {
                            uint32_t row = static_cast<uint32_t>(rec);   // младшая половина = строка
                            if (refresh || row == kPibNoRow) {
                                // Холодная ветка: запись новая (её положил AddEntityToBatches)
                                // либо состав изменился и кэш целиком недействителен.
                                const Entity e = static_cast<Entity>(rec >> 32);
                                row = (e < row_of.size()) ? row_of[e] : kPibNoRow;
                                rec = static_cast<uint64_t>(e) << 32 | row;
                            }
#ifndef NDEBUG
                            else {
                                // Кэш обязан совпадать с таблицей: расхождение значит, что состав
                                // сущностей изменился без BumpEntityRevision. Проявилось бы это
                                // не крашем, а чужой матрицей у одного объекта из миллиона.
                                const Entity e = static_cast<Entity>(rec >> 32);
                                assert(row == ((e < row_of.size()) ? row_of[e] : kPibNoRow));
                            }
#endif
                            // kPibNoRow(= -1) значит одно из двух, и оба пишутся в PIB одинаково:
                            //  — transformless: энтити батчится, но строки в transform/instance/
                            //    sphere не имеет (их домен {Positions ∧ Draw}) — такой sp строит
                            //    позицию сам (напр. скайбокс из камеры), строку никто не читает;
                            //  — протухший слот: энтити уже нет в сцене.
                            // Такая запись остаётся «незаполненной» и переспрашивается каждую
                            // заливку — второй сентинел ради неё не нужен: таких сущностей единицы.
                            // Именно -1, а НЕ пропуск: пропуск сдвинул бы последующие записи
                            // относительно firstInstance батчей. -1 понимают и culling-шейдер
                            // (скаттерит безусловно), и вершинник (гард по row).
                            dst[n++] = row;
                        }
                    }
}

uint32_t PIB_DataModule::CalculateEntityToCmd(PassManager* rm, uint64_t revision, uint8_t slot)
{
    if (revision == e2c_last_revision[slot]) return 0;
    e2c_last_revision[slot] = revision;
    // Один uint на PIB-запись — тот же размер, что PIB. Счётчик СВОЙ, а не total_elements:
    // гейты у буферов раздельные, полагаться на то, что PIB пересчитался в этом же кадре, нельзя.
    e2c_elements = ComputeElementCount(rm);
    return e2c_elements * sizeof(uint32_t);
}

void PIB_DataModule::StoreEntityToCmd(BufferManager* bm, PassManager* rm, UploadTask* task)
{
    // Тот же обход, что StorePIB/FinalizeOffsets, и та же нумерация команд, что в слепке:
    // индекс ЛОКАЛЬНЫЙ для ПРОХОДА (счётчик свой на проход), потому что каллинг адресует
    // команду от базы региона своего прохода, а не от начала буфера.
    uint32_t* dst = static_cast<uint32_t*>(
        bm->AcquireTransferWritePtr(task, e2c_elements * sizeof(uint32_t)));
    if (!dst) return;

    uint32_t n = 0;
    for (RenderPassStep* rp : rm->GetOrderedRenderPasses()) {
        uint32_t cmd_idx = 0;
        for (const auto& [_, sb] : rp->shader_batches)
            for (const auto& [_, ab] : sb.atlases_batches)
                for (const auto& [_, tb] : ab.texture_batches)
                    for (const auto& [_, mb] : tb.model_batches) {
                        const size_t cnt = mb.pib_sub_buffer.size();
                        if (n + cnt <= e2c_elements) {   // страховка, см. StorePIB
                            std::fill_n(dst + n, cnt, cmd_idx);
                            n += safe_u32(cnt);
                        }
                        cmd_idx++;
                    }
    }
}
