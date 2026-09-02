// Атмосфера кадра: линейный туман по ДИСТАНЦИИ от камеры. Ничего до start_distance, полный цвет
// тумана на full_distance, равномерный подъём между ними.
//
// Дистанция берётся как длина view-позиции пикселя, а не как проекция на ось взгляда: туман тогда
// нарастает СФЕРИЧЕСКИ (в углах кадра ровно так же, как в центре), и обратное преобразование в мир
// не нужно вовсе — ни позиции камеры, ни инверсии view.
//
// НЕБО туманится наравне с геометрией: его глубина = 1.0, то есть дальняя плоскость, и она заведомо
// дальше full_distance — скайбокс уходит в сплошной цвет тумана. Так у стены нет разрыва по
// горизонту: пиксель без геометрии красится тем же цветом, что и геометрия рядом с ним. Цена
// известная и принятая: при включённом тумане неба не видно.
//
// max_opacity = 0 выключает эффект целиком: проход становится тождеством, шаг остаётся.
//
// ПОРЯДОК: проход стоит ПОСЛЕ bloom-композита (26). Стоя перед ним, туман доводил пиксель до цвета
// стены, а composite подмешивал поверх свечение из scene_emission — таргета, который экранный
// проход не гасит, — и далёкая геометрия не исчезала, а оставалась размытым свечением поверх
// тумана. Здесь гасится уже собранный кадр вместе со свечением; писать в эмиссию не нужно.

[[vk::combinedImageSampler]]
Texture2D<float>  u_depth  : register(t0, space0);
[[vk::combinedImageSampler]]
SamplerState      u_depthS : register(s0, space0);

struct CameraData { float4x4 view; float4x4 proj; };
StructuredBuffer<CameraData> Camera : register(t1, space0);   // ro-буфер идёт ПОСЛЕ сэмплеров

// [[vk::image_format]]: DXC не выводит формат storage-образа из float4 (молча даёт rgba32f) —
// обязан совпадать с текстурой (rgba16f, см. TexturePresets::SceneHDR), иначе UB.
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> u_scene : register(u0, space1);

cbuffer FogParams : register(b0, space2) {
    float3 fog_color;       // цвет тумана, как есть
    float  start_distance;  // ближе — тумана нет вовсе
    float  full_distance;   // дальше — только цвет тумана
    float  max_opacity;     // потолок: 1 — даль исчезает полностью; 0 — эффект выключен
};

#include "comp/view_from_depth.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint w, h; u_scene.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) return;
    if (max_opacity <= 0.0) return;   // выключено: проход становится тождеством

    const float2 uv = (float2(tid.xy) + 0.5) / float2(w, h);
    const float  d  = u_depth.SampleLevel(u_depthS, uv, 0);

    // Евклидова дистанция до пикселя. Для неба (d = 1.0) это дальняя плоскость — она заведомо
    // за full_distance, поэтому скайбокс закрашивается полностью (см. шапку).
    const float dist = length(ViewPos(uv, d));

    // Знаменатель клампуем: full_distance <= start_distance — законная настройка «стена ровно на
    // start», и делить на ноль тут нечем.
    const float span = max(full_distance - start_distance, 1e-4);
    const float f    = saturate((dist - start_distance) / span) * saturate(max_opacity);

    u_scene[tid.xy] = float4(lerp(u_scene[tid.xy].rgb, fog_color, f), 1.0);
}
