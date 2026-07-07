// Обнуляет num_instances у ВСЕХ per-camera команд ПЕРЕД scatter — КАЖДЫЙ рендер.
// Зачем: scatter атомарно НАБИРАЕТ num_instances, а единственный сброс (заливка num=0) идёт
// лишь в prepare-фазе (sim). На 1М sim не успевает, и рендер-поток перерисовывает один и тот же
// слот БЕЗ re-prepare → scatter копит num_instances поверх старого → num становится кратен
// (2×,3×,10× …) реальному числу выживших, и одна и та же строка пишется в несколько слотов
// out_pib → ДУБЛИКАТЫ → объекты рисуются копиями друг друга (не тот размер/позиция). На 10k
// sim успевает, каждый рендер получает свежесброшенный слот — потому баг только на 1М.
//
// Этот clear — ПЕРВЫЙ compute-пасс в CULLING_PASS (создаётся раньше scatter → shader_batch[0]).
// SDL_GPU барьерит между compute-пассами в одном cb (как в bloom), поэтому scatter видит уже нули.
// Раскладка cbuffer = CullParams scatter'а (переиспользуем; нужны num_cameras и total_commands).

RWByteAddressBuffer Indirect : register(u0, space1);

cbuffer ClearParams : register(b0, space2) {
    uint total_slots;   // (1+L) * total_commands — всего (камера,команда) слотов индиректа
};

static const uint CMD_STRIDE = 20u;   // sizeof(SDL_GPUIndexedIndirectDrawCommand); num_instances @ 4

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint idx = tid.x;                       // индекс (камера,команда) = c*TC + k
    if (idx >= total_slots) return;
    Indirect.Store(idx * CMD_STRIDE + 4u, 0u);   // num_instances = 0
}
