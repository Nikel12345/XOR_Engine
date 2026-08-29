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

// ── Потолки раскладки вариантов ──
// Приходят ДЕФАЙНАМИ (Engine::InitDefaultShaders, они же в ключе кэша .spv). Дефолты держат
// сборку без них и ОБЯЗАНЫ совпадать с C++ (ShaderTypes.h).
#ifndef MAX_SLOTS
#define MAX_SLOTS 12
#endif
#ifndef MAX_VARIATIVE_SLOTS
#define MAX_VARIATIVE_SLOTS 4
#endif
#ifndef MAX_UVL_BLOCKS
#define MAX_UVL_BLOCKS 32
#endif

// ── Униформы (space3): b0 = UVL слотов материала (albedo), b1 = MaterialBlock (UIMaterialParams),
//    b2 = раскладка таблицы UVL (варианты). Порядок фиксирован пушем в ExecuteRenderBatches. ──
struct TextureData { uint4 data; };
// Таблица СГРУППИРОВАНА ПО СЛОТАМ: подряд все блоки слота, внутри группы [0] — дефолт. Индекс
// блока слота НЕ равен номеру слота — только через TexIndex. У материала без вариантов
// вырождается в прежний плотный UVL по required_slots (у UI это один albedo).
cbuffer TextureUVLBlock : register(b0, space3) { TextureData textures[MAX_UVL_BLOCKS]; };
cbuffer MaterialBlock   : register(b1, space3) { float4 bg_color; float4 text_color; float4 text_params; };
// text_params.x = text_height (доля высоты ректа под строку), .y = text_anchor (0=верх..1=низ).
cbuffer VariantLayoutBlock : register(b2, space3) {
    uint4 slot_layout[MAX_SLOTS / 4];   // (base<<16)|(cell<<8)|count на слот
    uint  material_index;               // offset 48 — массив кончается на 16-байтной границе
};

// ПЕРЕКЛЮЧЕНИЕ вариантов — opt-in по TEXTURE_VARIANTS (ставит Engine своим ui_fs). Без него
// шейдер собирается без обоих буферов и показывает ДЕФОЛТ слота: адресация через base остаётся
// (она в пуше), пропадает только выбор. Симметрично прологу main-пасса.
#ifdef TEXTURE_VARIANTS
// 2 сэмплера + 5 текст-буферов (t2..t6) → префикс t7, состояния t8. Строку трансформа
// вершинник уже отдаёт (v_row — он же адресует разреженный текст-канал), новых полей не нужно.
StructuredBuffer<int>  TexStatePrefix : register(t7, space2);
StructuredBuffer<uint> TexState       : register(t8, space2);
#endif

// Индекс блока UVL для слота s. Три проверки с разными ролями: g_stateOfs >= 0 — безопасность
// (нет элемента → индекс ушёл бы в минус), v >= count — корректность (номер протух), count > 1 —
// стоимость (иначе storage читал бы каждый пиксель каждого материала). См. main_pass/material_api.
uint TexIndex(uint s, int state_ofs)
{
    uint L     = slot_layout[s >> 2][s & 3];
    uint count = L & 0xFFu;
    uint v = 0u;
#ifdef TEXTURE_VARIANTS
    if (count > 1u && state_ofs >= 0)
        v = TexState[state_ofs + material_index * MAX_VARIATIVE_SLOTS + ((L >> 8) & 0xFFu)];
    if (v >= count) v = 0u;
#endif
    return (L >> 16) + v;
}

float4 sampleAlbedo(float2 uv, uint row)
{
    // Префикс читаем здесь, а не в вершиннике: буфер в ВЕРШИННОМ списке обязана была бы биндить
    // каждая sp с этим вершинником. row у нас уже есть — им же адресуется текст-канал.
#ifdef TEXTURE_VARIANTS
    const int state_ofs = TexStatePrefix[row];
#else
    const int state_ofs = -1;
#endif
    const uint b = TexIndex(0, state_ofs);   // 0 — единственный слот UI-материала (Albedo)
    float2 off = unpackUnorm2x16(textures[b].data.x);
    float2 scl = unpackUnorm2x16(textures[b].data.y);
    uint   lay = textures[b].data.z;
    return u_albedo.Sample(u_albedoSampler, float3(uv * scl + off, float(lay)));
}

float4 main(PSInput input) : SV_Target0
{
    // Фон: albedo × тинт.
    float4 bg = sampleAlbedo(input.v_uv, input.v_row) * bg_color;
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
