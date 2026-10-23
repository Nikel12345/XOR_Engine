// Переписывает команды прохода после scatter: поток на (блок, команда). Команда рисует кусок
// out_pib своего группового уровня: first_instance = его начало, num_instances = его счётчик.

#ifndef GROUP_LEVEL_BITS
#error GROUP_LEVEL_BITS
#endif

struct GroupLayout {
    uint  size;
    uint  out_offset;
    uint  first_group_level;
    uint  lod_count;
    uint  model;
};

StructuredBuffer<uint>        CommandToGroupLevel : register(t0, space0);   // команда -> (группа << GROUP_LEVEL_BITS) | уровень
StructuredBuffer<GroupLayout> GroupLayouts        : register(t1, space0);
StructuredBuffer<uint>        Counters            : register(t2, space0);

RWByteAddressBuffer Indirect : register(u0, space1);

cbuffer FixupParams : register(b0, space2) {
    uint num_blocks;
    uint cmd_base;
    uint commands;
    uint first_cmd;   // первая команда прохода в CommandToGroupLevel
    uint out_base;
    uint out_capacity;
    uint cnt_base;
    uint group_level_count;
};

static const uint CMD_STRIDE = 20u;   // sizeof(SDL_GPUIndexedIndirectDrawCommand); num_instances@4, first_instance@16

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= num_blocks * commands) return;
    uint b = tid.x / commands;
    uint c = tid.x - b * commands;

    uint word = CommandToGroupLevel[first_cmd + c];
    GroupLayout g = GroupLayouts[word >> GROUP_LEVEL_BITS];
    uint L = word & ((1u << GROUP_LEVEL_BITS) - 1u);

    uint cmd = cmd_base + b * commands + c;
    Indirect.Store(cmd * CMD_STRIDE + 4u,  Counters[cnt_base + b * group_level_count + g.first_group_level + L]);
    Indirect.Store(cmd * CMD_STRIDE + 16u, out_base + b * out_capacity + g.out_offset + L * g.size);
}
