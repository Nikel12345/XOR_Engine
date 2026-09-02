#include "main_pass/pass_targets.hlsl"

// Скайбокс: unlit-фон по env-кубмапе. Сэмплеры — ГЛОБАЛКИ MAIN_PASS (биндит пасс, не материал):
// слот 0 — тень, слот 1 — env (см. main_pass.frag.hlsl). Тень скайбоксу не нужна, но объявлена
// и «заякорена» веткой в main(): DXC стрипает неиспользуемые ресурсы, а SDL GPU ждёт ПЛОТНЫЕ
// слоты от 0 (num_samplers из рефлексии — СЧЁТ, биндинги 0..N-1) → без якоря env съехал бы
// в несуществующий слот 0 с чужим типом текстуры.

[[vk::combinedImageSampler]]
Texture2DArray<float>     u_shadowDepthArray  : register(t0, space2);
[[vk::combinedImageSampler]]
SamplerComparisonState    u_shadowSampler     : register(s0, space2);

[[vk::combinedImageSampler]]
TextureCube  u_envCube    : register(t1, space2);
[[vk::combinedImageSampler]]
SamplerState u_envSampler : register(s1, space2);

struct PSInput
{
    float4 sv_pos : SV_Position;
    [[vk::location(0)]] float3 v_dir : TEXCOORD0;
};

struct PSOutput
{
    MAIN_PASS_TARGETS
};

PSOutput main(PSInput input)
{
    PSOutput o;
    o.color    = float4(u_envCube.Sample(u_envSampler, normalize(input.v_dir)).rgb, 1.0);
    o.emission = float4(0.0, 0.0, 0.0, 0.0);
    o.ambient  = float4(0.0, 0.0, 0.0, 0.0);   // небо экранным AO не затеняется

    // ЯКОРЬ t0/s0: ветка никогда не выполняется (sv_pos.x >= 0 во вьюпорте), но компилятор
    // доказать этого не может → ресурс тени не стрипается, слоты остаются плотными.
    if (input.sv_pos.x < -1.0)
        o.color.r += u_shadowDepthArray.SampleCmpLevelZero(u_shadowSampler, float3(0.0, 0.0, 0.0), 0.0);

    return o;
}
