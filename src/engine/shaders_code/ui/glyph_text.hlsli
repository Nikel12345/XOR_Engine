#ifndef UI_GLYPH_TEXT_HLSL
#define UI_GLYPH_TEXT_HLSL

// Подключаемый текст-модуль UI. Разреженный текст-канал: presence-бит на row + пословный
// префикс-popcount (rank за O(1)) + компактный index {offset,count} + пул кодов глифов +
// буфер глифов (GlyphUVL). Модуль даёт ХЕЛПЕРЫ; сами буферы объявляет вызывающий (ui.frag)
// с нужными регистрами — модуль не диктует регистры, переносим.

#include "main_pass/math.hlsl"   // unpackUnorm2x16

// Есть ли текст у row (по уже загруженному слову presence-маски).
bool UIHasText(uint word, uint bit) { return ((word >> bit) & 1u) != 0u; }

// rank(row) = компактный индекс в index[] = число единиц presence ДО этого row.
uint UITextRank(uint word, uint wordBase, uint bit) {
    return wordBase + countbits(word & ((1u << bit) - 1u));
}

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
