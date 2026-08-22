// Bloom composite + tonemap. Завершает кадр: подмешивает размытую пирамиду (bloom_0) в HDR-сцену
// и сжимает HDR→LDR (Reinhard). Читает scene_hdr через storage (point, тот же размер), bloom_0 —
// через LINEAR-сэмплер (мельче, апскейл). Пишет результат обратно в scene_hdr (RMW). scene_hdr
// здесь только storage, bloom_0 только sampler → нет одновременного sampler+storage на одной текстуре.
// present-проход затем блитит scene_hdr (уже LDR) в свопчейн.

[[vk::combinedImageSampler]]
Texture2D<float4>   u_bloom   : register(t0, space0);
[[vk::combinedImageSampler]]
SamplerState        u_sampler : register(s0, space0);

// [[vk::image_format]]: DXC не выводит формат storage-образа из float4 (молча даёт rgba32f) —
// формат обязан совпадать с текстурой (rgba16f, см. TexturesPresets::SceneHDR), иначе UB.
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> u_scene : register(u0, space1);

cbuffer BloomParams : register(b0, space2) {
    uint  useKaris;
    float intensity;   // доля подмешивания bloom (~0.05)
    float threshold;   // не используется
    float knee;        // не используется
    float clampMax;   // потолок вклада сцены (prefilter); в down/up/composite не используется
};

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint w, h; u_scene.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) return;

    float2 uv = (float2(tid.xy) + 0.5) / float2(w, h);

    float3 scene = u_scene[tid.xy].rgb;
    float3 bloom = u_bloom.SampleLevel(u_sampler, uv, 0).rgb;
    float3 hdr   = scene + bloom * intensity;

    // Hue-preserving clip: если хоть один канал > 1, делим ВЕСЬ цвет на максимальный канал —
    // пропорции (= цвет) сохраняются, в белый цвет НЕ уходит (в белый — только если сама эмиссия
    // белёсая, т.е. каналы ~равны). База (max ≤ 1) не трогается → потускнения нет. В отличие от
    // поканального клампа present'а, который режет каждый канал отдельно и сдвигает хью.
    float m = max(hdr.r, max(hdr.g, hdr.b));
    if (m > 1.0) hdr /= m;

    u_scene[tid.xy] = float4(hdr, 1.0);
}
