#ifndef UI_GLYPH_TEXT_HLSL
#define UI_GLYPH_TEXT_HLSL

// Подключаемый текст-модуль UI. Текст — разреженный по row канал: адресацию (presence-бит +
// пословный rank) даёт общий SPARSE_CHANNEL, данные канала — компактный index {offset,count},
// пул кодов глифов и буфер глифов (GlyphUVL). Модуль даёт ХЕЛПЕРЫ и подтягивает механизм;
// сами буферы объявляет вызывающий (ui.frag) с нужными регистрами — модуль их не диктует.

#include "main_pass/math.hlsl"   // unpackUnorm2x16
#include "sparse_rank.hlsli"     // SPARSE_CHANNEL: тот же механизм, что у канала состояний

// Покрытие глифа: сэмпл __TextAtlas (R8) по UVL глифа и локальной координате внутри его ячейки.
// glyphUvl = TextureData.data (x=packed offset, y=packed scale, z=layer, w=advance-биты — не тут).
float UIGlyphCoverage(Texture2DArray<float> atlas, SamplerState samp, uint4 glyphUvl, float2 local) {
    if (glyphUvl.y == 0u) return 0.0;   // пустой глиф (пробел): нулевой scale → ничего не рисуем
    float2 off = unpackUnorm2x16(glyphUvl.x);
    float2 scl = unpackUnorm2x16(glyphUvl.y);
    uint   lay = glyphUvl.z;
    return atlas.Sample(samp, float3(local * scl + off, float(lay))).r;
}

#endif
