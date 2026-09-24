// Переписывает команды прохода после scatter: поток на (блок, команда). Команда рисует кусок
// out_pib своего группового уровня: first_instance = его начало, num_instances = его счётчик.

struct GroupEntry {
    uint  size;
    uint  out_offset;
    uint  gl_base;
    uint  lod_count;
    float switches[3];
    uint  pad;
};

StructuredBuffer<uint>       CmdGroupLevel : register(t0, space0);   // команда -> (группа << 2) | уровень
StructuredBuffer<GroupEntry> Groups        : register(t1, space0);
StructuredBuffer<uint>       Counters      : register(t2, space0);

RWByteAddressBuffer Indirect : register(u0, space1);

cbuffer FixupParams : register(b0, space2) {
    uint num_blocks;
    uint cmd_base;
    uint commands;
    uint first_cmd;   // первая команда прохода в CmdGroupLevel
    uint out_base;
    uint out_cap;
    uint cnt_base;
    uint gl_count;
};

static const uint CMD_STRIDE = 20u;   // sizeof(SDL_GPUIndexedIndirectDrawCommand); num_instances@4, first_instance@16

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= num_blocks * commands) return;
    uint b = tid.x / commands;
    uint c = tid.x - b * commands;

    uint word = CmdGroupLevel[first_cmd + c];
    GroupEntry g = Groups[word >> 2];
    uint L = word & 3u;

    uint cmd = cmd_base + b * commands + c;
    Indirect.Store(cmd * CMD_STRIDE + 4u,  Counters[cnt_base + b * gl_count + g.gl_base + L]);
    Indirect.Store(cmd * CMD_STRIDE + 16u, out_base + b * out_cap + g.out_offset + L * g.size);
}
