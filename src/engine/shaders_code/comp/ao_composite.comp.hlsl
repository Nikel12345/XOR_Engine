// Применение экранного AO. Ключевая строка одна:
//
//     scene_hdr -= ambient * (1 - AO)
//
// В scene_ambient main-проход продублировал ту долю цвета, которую внёс НЕпрямой свет (подъём
// ambient-полом над прямым светом + отражение окружения, см. main_pass.frag.hlsl). Поэтому при
// AO = 1 вычитается ноль — кадр совпадает с прежним до бита; при AO = 0 диффуз садится ровно на
// прямой свет. Прямой свет и эмиссия не участвуют вовсе: AO — затенение непрямого, не света ламп.
//
// Кто в ambient записал ноль (небо, фракталы — у них своё затенение), тем композит ничего не
// меняет: вычитается ноль независимо от AO.
//
// Порядок в кадре важен: проход стоит между MAIN (20) и TRANSPARENT (22) — ПОСЛЕ него прозрачные
// и дебаг рисуются поверх (их AO не касается, их ambient в таргет и не попадал), а bloom (26)
// увидит уже затенённую сцену и тонмаппит её.

[[vk::combinedImageSampler]]
Texture2D<float4> u_ambient  : register(t0, space0);
[[vk::combinedImageSampler]]
SamplerState      u_ambientS : register(s0, space0);
// AO — ПОЛОВИННОГО разрешения, поэтому читается LINEAR-сэмплером: билинейный апскейл. Кромки
// затенения и так размыты билатеральным блюром, отдельный bilateral-upsample ничего не добавит.
[[vk::combinedImageSampler]]
Texture2D<float>  u_ao       : register(t1, space0);
[[vk::combinedImageSampler]]
SamplerState      u_aoS      : register(s1, space0);

// Формат обязан совпадать с текстурой (rgba16f, TexturePresets::SceneHDR) — DXC его не выводит.
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> u_scene : register(u0, space1);

cbuffer AOParams : register(b0, space2) {
    float radius;
    float intensity;   // 0 = AO выключен: композит становится тождеством
    float power;
    float bias;
};

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint w, h; u_scene.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) return;
    if (intensity <= 0.0) return;

    const float2 uv = (float2(tid.xy) + 0.5) / float2(w, h);

    const float3 ambient = u_ambient.SampleLevel(u_ambientS, uv, 0).rgb;
    const float  ao      = u_ao.SampleLevel(u_aoS, uv, 0);

    const float3 hdr = u_scene[tid.xy].rgb - ambient * (1.0 - saturate(ao));
    u_scene[tid.xy] = float4(max(hdr, (float3)0.0), 1.0);
}
