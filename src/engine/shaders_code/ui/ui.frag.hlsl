// UI-фрагментник: обычный текстурный (albedo как main_pass, но БЕЗ освещения) + композит текста.
// Фон = bg_color × albedo. Текст = text_color, замаскированный покрытием глифа из __TextAtlas
// (разреженный канал по row). depth_write включён, поэтому прозрачные пиксели ОТБРАСЫВАЕМ (clip),
// чтобы перекрытие решала z, а не порядок отрисовки (см. решение по прозрачности UI).

#include "ui/glyph_text.hlsli"

struct PSInput
{
    float4 sv_pos                    : SV_Position;
    [[vk::location(0)]] float2 v_uv  : TEXCOORD0;
    [[vk::location(1)]] float  v_alpha : TEXCOORD1;
    [[vk::location(2)]] nointerpolation uint v_row : TEXCOORD2;
};

// Глобалка прохода: __TextAtlas (R8, покрытие/ глифов) — слот 0.
[[vk::combinedImageSampler]] Texture2DArray<float> u_glyph        : register(t0, space2);
[[vk::combinedImageSampler]] SamplerState          u_glyphSampler : register(s0, space2);
// Слот материала: фон (albedo) — слот 1 (после глобалки).
[[vk::combinedImageSampler]] Texture2DArray u_albedo        : register(t1, space2);
[[vk::combinedImageSampler]] SamplerState   u_albedoSampler : register(s1, space2);

// ── Storage-буферы (нумеруются ПОСЛЕ сэмплеров: 2 сэмплера → t2..). Разреженный текст-канал. ──
StructuredBuffer<uint>  TextBits     : register(t2, space2);   // presence, бит/row
StructuredBuffer<uint>  TextWordBase : register(t3, space2);   // пословный префикс-popcount
StructuredBuffer<uint2> TextIndex    : register(t4, space2);   // {offset,count} на текст-элемент (индекс = rank)
StructuredBuffer<uint>  TextPool     : register(t5, space2);   // коды глифов подряд
StructuredBuffer<uint4> GlyphUVL     : register(t6, space2);   // code → UVL глифа (.w = advance-биты)

// ── Униформы (space3): b0 = UVL слотов материала (albedo), b1 = MaterialBlock (UIMaterialParams). ──
struct TextureData { uint4 data; };
cbuffer TextureUVLBlock : register(b0, space3) { TextureData textures[1]; };   // [0] = albedo UVL
cbuffer MaterialBlock   : register(b1, space3) { float4 bg_color; float4 text_color; float4 text_params; };
// text_params.x = text_height (доля высоты ректа под строку), .y = text_anchor (0=верх..1=низ).

float4 sampleAlbedo(float2 uv)
{
    float2 off = unpackUnorm2x16(textures[0].data.x);
    float2 scl = unpackUnorm2x16(textures[0].data.y);
    uint   lay = textures[0].data.z;
    return u_albedo.Sample(u_albedoSampler, float3(uv * scl + off, float(lay)));
}

float4 main(PSInput input) : SV_Target0
{
    // Фон: albedo × тинт.
    float4 bg = sampleAlbedo(input.v_uv) * bg_color;
    bg.a *= input.v_alpha;

    // Текст: покрытие глифа из разреженного канала по row.
    float coverage = 0.0;
    uint row = input.v_row;
    uint w = row >> 5u, b = row & 31u;
    uint word = TextBits[w];

    if (UIHasText(word, b)) {
        uint rank  = UITextRank(word, TextWordBase[w], b);
        uint2 span = TextIndex[rank];        // x=offset, y=count
        uint count = span.y;
        if (count > 0u) {
            // Раскладка/посадку/форму узла считает Yoga (UI_Yoga) — rect УЖЕ правильной формы, поэтому
            // шейдер просто ЗАЛИВАЕТ узел строкой: пропорционально advance по X, uv.y напрямую по Y.
            // Единицы advance сокращаются (ratio), поэтому нормировка в GlyphUVL[code].w несущественна.
            float totalAdv = 0.0;
            for (uint i = 0u; i < count; ++i)
                totalAdv += asfloat(GlyphUVL[TextPool[span.x + i]].w);

            if (totalAdv > 0.0) {
                float targetX = input.v_uv.x * totalAdv;
                float accum   = 0.0;
                [loop] for (uint i = 0u; i < count; ++i) {
                    uint  code = TextPool[span.x + i];
                    float a    = asfloat(GlyphUVL[code].w);
                    if (targetX < accum + a) {
                        float localX = (a > 0.0) ? (targetX - accum) / a : 0.0;   // 0..1 в advance-слоте
                        coverage = UIGlyphCoverage(u_glyph, u_glyphSampler, GlyphUVL[code], float2(localX, input.v_uv.y));
                        break;
                    }
                    accum += a;
                }
            }
        }
    }

    // Композит: текст поверх фона, замаскирован покрытием.
    float3 rgb = lerp(bg.rgb, text_color.rgb, coverage * text_color.a);
    float  a   = max(bg.a, coverage * text_color.a);

    clip(a - 0.004);   // depth_write ON → прозрачные пиксели не пишут ни глубину, ни цвет
    return float4(rgb, a);
}
