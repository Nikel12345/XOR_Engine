#ifndef UI_NINE_SLICE_API_HLSL
#define UI_NINE_SLICE_API_HLSL

// Контракт пользовательской части и программы — docs/ui.md, «Картинка из 9 частей».

#include "main_pass/math.hlsl"

// Выход ui.vert.
struct PSInput
{
    float4 sv_pos                      : SV_Position;
    [[vk::location(0)]] float2 v_uv    : TEXCOORD0;
    [[vk::location(1)]] float  v_alpha : TEXCOORD1;
    [[vk::location(2)]] nointerpolation uint v_row : TEXCOORD2;
};

// Слот 0 — глобальная текстура UI-прохода (TextAtlas). Шейдер её не читает, но удерживает якорем
// в ui/nine_slice.frag.hlsl.
[[vk::combinedImageSampler]] Texture2DArray<float> u_glyph        : register(t0, space2);
[[vk::combinedImageSampler]] SamplerState          u_glyphSampler : register(s0, space2);
[[vk::combinedImageSampler]] Texture2DArray        u_albedo        : register(t1, space2);
[[vk::combinedImageSampler]] SamplerState          u_albedoSampler : register(s1, space2);

StructuredBuffer<float4x4> ModelMatrixBlock : register(t2, space2);

// Значение приходит дефайном (DefaultShaderSet). Дефолт держит пользовательские шейдеры, собранные
// без него, и обязан совпадать с C++ (ShaderTypes.h).
#ifndef MAX_UVL_BLOCKS
#define MAX_UVL_BLOCKS 32
#endif
struct TextureData { uint4 data; };
//@push uvl
cbuffer TextureUVLBlock : register(b0, space3) { TextureData textures[MAX_UVL_BLOCKS]; };

// Порядок маркеров в тексте = порядок слотов, поэтому слот MaterialBlock раздаёт пролог.
//@push material_params
#define MATERIAL_BLOCK_REGISTER register(b1, space3)

//@push ui_window_size
cbuffer UIWindowBlock : register(b2, space3) { float2 u_windowSize; };

// Контракт с UI_Manager::EmitNode: матрица = to_ndc * local * размер узла, и to_ndc масштабирует
// ось z высотой окна, как y.
float2 NineSliceNodeSize(uint row)
{
    const float4x4 matrix_ndc = ModelMatrixBlock[row];
    const float3 ndc_to_pixels = 0.5 * float3(u_windowSize.x, u_windowSize.y, u_windowSize.y);
    const float3 axis_x = float3(matrix_ndc[0][0], matrix_ndc[1][0], matrix_ndc[2][0]) * ndc_to_pixels;
    const float3 axis_y = float3(matrix_ndc[0][1], matrix_ndc[1][1], matrix_ndc[2][1]) * ndc_to_pixels;
    return float2(length(axis_x), length(axis_y));
}

float NineSliceAxis(float pixel, float node_size, float image_size, float corner_size)
{
    const float begin = min(corner_size, 0.5 * node_size);
    if (pixel < begin) return pixel;
    if (pixel >= node_size - begin) return image_size - (node_size - pixel);
    return corner_size + fmod(pixel - corner_size, image_size - 2.0 * corner_size);
}

float4 SampleNineSlice(PSInput input)
{
    const uint4 albedo_uvl = textures[0].data;
    uint atlas_width, atlas_height, atlas_layers;
    u_albedo.GetDimensions(atlas_width, atlas_height, atlas_layers);
    const float2 atlas_size = float2(atlas_width, atlas_height);

    // Картинка лежит в атласе с целого текселя, а UVL хранит её в unorm16 с погрешностью до трёх
    // сотых текселя. Без округления выборка шла бы мимо центров текселей и подмешивала соседние.
    const float2 image_origin = round(unpackUnorm2x16(albedo_uvl.x) * atlas_size);
    const float2 image_size   = round(unpackUnorm2x16(albedo_uvl.y) * atlas_size);
    const float2 corner_size  = floor(image_size / 3.0);

    const float2 node_size = NineSliceNodeSize(input.v_row);
    const float2 pixel     = input.v_uv * node_size;
    float2 texel = float2(NineSliceAxis(pixel.x, node_size.x, image_size.x, corner_size.x),
                          NineSliceAxis(pixel.y, node_size.y, image_size.y, corner_size.y));
    texel = clamp(texel, 0.5, image_size - 0.5);
    return u_albedo.SampleLevel(u_albedoSampler,
                                float3((image_origin + texel) / atlas_size, float(albedo_uvl.z)), 0.0);
}

#endif
