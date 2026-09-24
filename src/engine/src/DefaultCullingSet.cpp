#include "PCH.h"
#include "DefaultCullingSet.h"
#include "DefaultRenderPassSet.h"
#include "EngineContext.h"
#include "BufferManager.h"
#include "PassManager.h"
#include "ShaderManager.h"
#include "BatchBuilder.h"
#include "ObjectManager.h"
#include "ModelManager.h"
#include "LightDataModule.h"
#include "BoundSphereDataModule.h"
#include "RenderCommandData.h"
#include "TextureData.h"

namespace {

constexpr const char* OUT_PIB_BUFFER         = "DefaultOutPibBuffer";
constexpr const char* COUNTERS_BUFFER        = "DefaultCullCountersBuffer";
constexpr const char* RECORD_GROUP_BUFFER    = "DefaultRecordGroupBuffer";
constexpr const char* GROUP_TABLE_BUFFER     = "DefaultGroupTableBuffer";
constexpr const char* CMD_GROUP_LEVEL_BUFFER = "DefaultCmdGroupLevelBuffer";
constexpr const char* BOUND_SPHERE_BUFFER    = "DefaultBoundSphereBuffer";

// Должно совпадать с GroupEntry в culling_scatter/culling_fixup.
struct GroupEntry {
    uint32_t size = 0;
    uint32_t out_offset = 0;
    uint32_t gl_base = 0;
    uint32_t lod_count = 1;
    float    switches[3] = {};
    uint32_t pad = 0;
};
static_assert(sizeof(GroupEntry) == 32);

struct alignas(16) ScatterPush {
    uint32_t range_start, range_count, num_blocks, out_base;
    uint32_t out_cap, cnt_base, gl_count, target_height;
    float    min_screen_radius_px;
    uint32_t pad[3];
};

struct alignas(16) FixupPush {
    uint32_t num_blocks, cmd_base, commands, first_cmd;
    uint32_t out_base, out_cap, cnt_base, gl_count;
};

struct alignas(16) ClearPush {
    uint32_t total;
    uint32_t pad[3];
};

// Раскладка прохода: из дерева (группы, уровни, команды) и из регионов слота (блоки, база команд).
struct PassLayout {
    uint32_t first_pib = 0, records = 0;
    uint32_t cmd_base = 0, commands = 0, first_cmd = 0;
    uint32_t blocks = 0, gl = 0, out_cap = 0;
    uint32_t out_base = 0, cnt_base = 0;
};

class CullingTables {
public:
    // Sim-поток, фаза размеров: дерево и регионы слота уже устоялись. Идемпотентно в пределах кадра.
    void Stamp(PassManager* pm, uint64_t revision, uint8_t slot)
    {
        if (revision != built_revision) Rebuild(pm, revision);

        const PassRegions& regions = pm->AskRegions(slot);
        std::vector<PassLayout>& layout = layouts[slot];
        layout = tree_layout;
        uint32_t out = 0, cnt = 0;
        for (size_t i = 0; i < layout.size(); ++i) {
            PassLayout& p = layout[i];
            if (i < regions.per_pass.size()) {
                const PassRegion& reg = regions.per_pass[i];
                p.blocks = reg.command_blocks_count;
                p.cmd_base = reg.cmd_base;
            }
            p.out_base = out;  out += p.blocks * p.out_cap;
            p.cnt_base = cnt;  cnt += p.blocks * p.gl;
        }
        total_out[slot] = out;
        total_counters[slot] = cnt;
    }

    const PassLayout& Pass(uint8_t slot, uint32_t ordinal) const
    {
        static const PassLayout none{};
        return ordinal < layouts[slot].size() ? layouts[slot][ordinal] : none;
    }
    uint32_t TotalOut(uint8_t slot) const { return total_out[slot]; }
    uint32_t TotalCounters(uint8_t slot) const { return total_counters[slot]; }

    // Заливки таблиц: гейт — ревизия батчей по слоту, как у PIB.
    uint32_t TableSize(const void* table, uint64_t revision, uint8_t slot)
    {
        uint64_t& last = uploaded[TableIndex(table)][slot];
        if (last == revision) return 0;
        if (table == &record_group)    return safe_u32(record_group.size() * sizeof(uint32_t));
        if (table == &groups)          return safe_u32(groups.size() * sizeof(GroupEntry));
        return safe_u32(cmd_group_level.size() * sizeof(uint32_t));
    }
    void StoreTable(BufferManager* bm, UploadTask* task, const void* table, uint64_t revision, uint8_t slot)
    {
        uint64_t& last = uploaded[TableIndex(table)][slot];
        if (last == revision) return;
        last = revision;
        if (table == &record_group && !record_group.empty())
            bm->UploadToTransferBuffer(task, safe_u32(record_group.size() * sizeof(uint32_t)), record_group.data());
        else if (table == &groups && !groups.empty())
            bm->UploadToTransferBuffer(task, safe_u32(groups.size() * sizeof(GroupEntry)), groups.data());
        else if (table == &cmd_group_level && !cmd_group_level.empty())
            bm->UploadToTransferBuffer(task, safe_u32(cmd_group_level.size() * sizeof(uint32_t)), cmd_group_level.data());
    }

    std::vector<uint32_t>   record_group;
    std::vector<GroupEntry> groups;
    std::vector<uint32_t>   cmd_group_level;

private:
    size_t TableIndex(const void* table) const
    {
        return table == &record_group ? 0 : (table == &groups ? 1 : 2);
    }

    // Обходы групп и дерева обязаны совпадать с FinalizeOffsets/StorePIB (порядок записей) и
    // StoreIndirect (порядок команд).
    void Rebuild(PassManager* pm, uint64_t revision)
    {
        built_revision = revision;
        record_group.clear();
        groups.clear();
        cmd_group_level.clear();
        tree_layout.clear();

        std::unordered_map<const DrawGroup*, uint32_t> group_index;
        uint32_t records = 0;
        for (RenderPassStep* rp : pm->GetOrderedRenderPasses()) {
            PassLayout p;
            p.first_pib = records;
            p.first_cmd = safe_u32(cmd_group_level.size());
            for (const auto& [_, group] : rp->draw_groups) {
                const uint32_t index = safe_u32(groups.size());
                group_index[&group] = index;
                GroupEntry e;
                e.size = safe_u32(group.records.size());
                e.out_offset = p.out_cap;
                e.gl_base = p.gl;
                e.lod_count = group.lod_count;
                for (int L = 0; L < 3; ++L) e.switches[L] = group.switches[L];
                groups.push_back(e);
                record_group.insert(record_group.end(), e.size, index);
                p.out_cap += e.size * e.lod_count;
                p.gl += e.lod_count;
                p.records += e.size;
            }
            for (const auto& [_, sb] : rp->shader_batches)
                for (const auto& [_, ab] : sb.atlases_batches)
                    for (const auto& [_, tb] : ab.texture_batches)
                        for (const auto& [_, mb] : tb.model_batches)
                            cmd_group_level.push_back((group_index[mb.group] << 2) | mb.level);
            p.commands = safe_u32(cmd_group_level.size()) - p.first_cmd;
            records += p.records;
            tree_layout.push_back(p);
        }
    }

    uint64_t built_revision = ~0ull;
    std::vector<PassLayout> tree_layout;
    std::vector<PassLayout> layouts[BUFFERING_LEVEL];
    uint32_t total_out[BUFFERING_LEVEL] = {};
    uint32_t total_counters[BUFFERING_LEVEL] = {};
    uint64_t uploaded[3][BUFFERING_LEVEL] = {
        { ~0ull, ~0ull, ~0ull }, { ~0ull, ~0ull, ~0ull }, { ~0ull, ~0ull, ~0ull } };
};

CullingTables         g_tables;
BoundSphereDataModule g_spheres;
bool                  g_enabled = false;

void CreateBuffers(EngineContext* ctx)
{
    BufferManager* bm = ctx->GetBufferManager();
    PassManager*   pm = ctx->GetPassManager();
    BatchBuilder*  bb = ctx->GetBatchBuilder();
    ObjectManager* om = ctx->GetObjectManager();
    ModelManager*  mm = ctx->GetModelManager();

    // Вершинники читают out_pib через подстановку в проходе, а не по своим спискам буферов, поэтому
    // сбор usage по спискам программ об этом чтении не узнает.
    bm->CreateBufferData(OUT_PIB_BUFFER, 4096, BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default)
        ->usage |= SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    bm->CreateBufferData(COUNTERS_BUFFER,        1024, BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default);
    bm->CreateBufferData(RECORD_GROUP_BUFFER,    4096, BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default);
    bm->CreateBufferData(GROUP_TABLE_BUFFER,     1024, BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default);
    bm->CreateBufferData(CMD_GROUP_LEVEL_BUFFER, 1024, BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default);
    bm->CreateBufferData(BOUND_SPHERE_BUFFER,    4096, BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default);

    auto stamp = [pm, bb](uint8_t slot) { g_tables.Stamp(pm, bb->BatchesRevision(), slot); };

    bm->CreateUpdateInstruction(OUT_PIB_BUFFER, nullptr,
        [bm, stamp]() -> uint32_t { const uint8_t s = bm->logic_index.load(); stamp(s); return g_tables.TotalOut(s) * sizeof(int32_t); });
    bm->CreateUpdateInstruction(COUNTERS_BUFFER, nullptr,
        [bm, stamp]() -> uint32_t { const uint8_t s = bm->logic_index.load(); stamp(s); return g_tables.TotalCounters(s) * sizeof(uint32_t); });

    auto table = [bm, bb, stamp](BufferDataName name, const void* data) {
        bm->CreateUpdateInstruction(name,
            [bb, data](SDL_GPUCopyPass*, BufferManager* bm, UploadTask& task) {
                g_tables.StoreTable(bm, &task, data, bb->BatchesRevision(), bm->logic_index.load()); },
            [bm, bb, stamp, data]() -> uint32_t {
                const uint8_t s = bm->logic_index.load(); stamp(s);
                return g_tables.TableSize(data, bb->BatchesRevision(), s); });
    };
    table(RECORD_GROUP_BUFFER,    &g_tables.record_group);
    table(GROUP_TABLE_BUFFER,     &g_tables.groups);
    table(CMD_GROUP_LEVEL_BUFFER, &g_tables.cmd_group_level);

    bm->CreateUpdateInstruction(BOUND_SPHERE_BUFFER,
        [om, mm](SDL_GPUCopyPass*, BufferManager* bm, UploadTask& task) {
            g_spheres.StoreSpheres(bm, &task, om, mm, om->EntityRevision() + mm->SpheresRevision(), bm->logic_index.load()); },
        [om, mm, bm]() -> uint32_t {
            return g_spheres.CalculateSphereSize(om, om->EntityRevision() + mm->SpheresRevision(), bm->logic_index.load()); });
}

void CreatePrograms(EngineContext* ctx)
{
    namespace RP = DefaultRenderPassNamespace;
    using namespace DefaultBuffersNames;
    ShaderManager* sm = ctx->GetShaderManager();
    PassManager*   pm = ctx->GetPassManager();
    const ResourceTag tags = ResourceTag::CodeOwned | ResourceTag::Default;

    ctx->CreateComputeShader("culling_clear_cs",   "../engine/shaders_code/comp/culling_clear.comp.hlsl", tags);
    ctx->CreateComputeShader("culling_scatter_cs", "../engine/shaders_code/comp/culling_scatter.comp.hlsl", tags);
    ctx->CreateComputeShader("culling_fixup_cs",   "../engine/shaders_code/comp/culling_fixup.comp.hlsl", tags);

    ctx->CreateComputeShaderProgram("csp_cull_clear", "culling_clear_cs", { COUNTERS_BUFFER }, {}, {}, {}, {}, RP::CULLING_PASS, tags);
    sm->CreateComputePushInstruction<RP::CullingState>("csp_cull_clear",
        [](const PushConstantBinder& b, RP::CullingState) { b.Push(ClearPush{ g_tables.TotalCounters(b.frame), {} }); });
    sm->CreateDispatchInstruction<RP::DummyDispatchData>("csp_cull_clear",
        [](DispatchSizeBinder& b, RP::DummyDispatchData) { b.Dispatch(g_tables.TotalCounters(b.frame)); });

    struct Culled { const char* pass; BufferDataName cameras; bool main; };
    const Culled culled[] = {
        { RP::SHADOW_PASS,      DEFAULT_LIGHT_CAMERA_BUFFER, false },
        { RP::MAIN_PASS,        DEFAULT_CAMERA_BUFFER,       true  },
        { RP::TRANSPARENT_PASS, DEFAULT_CAMERA_BUFFER,       false },
        { RP::DEBUG_PASS,       DEFAULT_CAMERA_BUFFER,       false },
        { RP::UI_PASS,          DEFAULT_CAMERA_BUFFER,       false },
    };
    TextureAtlas* lod_target = ctx->GetTextureAtlas(std::string("scene_hdr"));

    for (const Culled& c : culled) {
        RenderPassStep* rp = pm->GetRenderPassStep(c.pass);
        if (!rp) continue;
        rp->buffer_substitutes.push_back({ DEFAULT_POSITION_INDEX_BUFFER, OUT_PIB_BUFFER });
        const uint32_t ordinal = rp->ordinal;
        const std::string name = std::string("csp_cull_scatter_") + c.pass;

        ctx->CreateComputeShaderProgram(name, "culling_scatter_cs",
            { OUT_PIB_BUFFER, COUNTERS_BUFFER },
            { DEFAULT_POSITION_INDEX_BUFFER, RECORD_GROUP_BUFFER, GROUP_TABLE_BUFFER, BOUND_SPHERE_BUFFER,
              DEFAULT_TRANSFORM_BUFFER, c.cameras, DEFAULT_CAMERA_BUFFER },
            {}, {}, {}, RP::CULLING_PASS, tags);
        sm->CreateComputePushInstruction<RP::CullingState>(name,
            [ordinal, lod_target, main = c.main](const PushConstantBinder& b, RP::CullingState st) {
                const PassLayout& p = g_tables.Pass(b.frame, ordinal);
                ScatterPush push{};
                push.range_start = p.first_pib;
                push.range_count = p.records;
                push.num_blocks = p.blocks;
                push.out_base = p.out_base;
                push.out_cap = p.out_cap;
                push.cnt_base = p.cnt_base;
                push.gl_count = p.gl;
                push.target_height = lod_target ? lod_target->height : 0u;
                // Мелочь отсекается только у прохода камеры игрока: у теней своё разрешение.
                push.min_screen_radius_px = main ? st.min_screen_radius_px : 0.0f;
                b.Push(push);
            });
        sm->CreateDispatchInstruction<RP::DummyDispatchData>(name,
            [ordinal](DispatchSizeBinder& b, RP::DummyDispatchData) {
                const PassLayout& p = g_tables.Pass(b.frame, ordinal);
                b.Dispatch(p.blocks ? p.records : 0u);
            });
    }

    // Fixup — после ВСЕХ scatter: порядок создания программ = порядок исполнения.
    for (const Culled& c : culled) {
        RenderPassStep* rp = pm->GetRenderPassStep(c.pass);
        if (!rp) continue;
        const uint32_t ordinal = rp->ordinal;
        const std::string name = std::string("csp_cull_fixup_") + c.pass;

        ctx->CreateComputeShaderProgram(name, "culling_fixup_cs",
            { DEFAULT_INDIRECT_BUFFER },
            { CMD_GROUP_LEVEL_BUFFER, GROUP_TABLE_BUFFER, COUNTERS_BUFFER },
            {}, {}, {}, RP::CULLING_PASS, tags);
        sm->CreateComputePushInstruction<RP::CullingState>(name,
            [ordinal](const PushConstantBinder& b, RP::CullingState) {
                const PassLayout& p = g_tables.Pass(b.frame, ordinal);
                b.Push(FixupPush{ p.blocks, p.cmd_base, p.commands, p.first_cmd,
                                  p.out_base, p.out_cap, p.cnt_base, p.gl });
            });
        sm->CreateDispatchInstruction<RP::DummyDispatchData>(name,
            [ordinal](DispatchSizeBinder& b, RP::DummyDispatchData) {
                const PassLayout& p = g_tables.Pass(b.frame, ordinal);
                b.Dispatch(p.blocks * p.commands);
            });
    }
}

} // namespace

void DefaultCullingSet::Enable(EngineContext* ctx, LightDataModule* ldm)
{
    if (g_enabled) { SDL_Log("DefaultCullingSet::Enable: already enabled"); return; }
    g_enabled = true;

    CreateBuffers(ctx);
    CreatePrograms(ctx);

    // Каждой световой камере — свои копии команд тени: у каждой свой результат отсева.
    ctx->GetPassManager()->CreateRegionCountInstruction(DefaultRenderPassNamespace::SHADOW_PASS,
        [ldm](uint8_t slot) { return ldm->AskNumLightCameras(slot); });
}
