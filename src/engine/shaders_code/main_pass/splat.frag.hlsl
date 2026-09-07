// Фрагментник СПЛАТ-прохода — ЗАГЛУШКА на время сборки конвейера: постоянный цвет, чтобы сплаты
// было видно глазом и можно было проверить, что объект действительно «ушёл там и пришёл здесь».
// Настоящая версия обязана быть фрагментником МАТЕРИАЛА (см. splat.vert.hlsl).
#include "main_pass/pass_targets.hlsl"

struct PSInput  { float4 sv_pos : SV_Position; };
struct PSOutput { MAIN_PASS_TARGETS };

PSOutput main(PSInput input)
{
    PSOutput o;
    o.color    = float4(1.0, 0.0, 1.0, 1.0);   // маджента: заведомо не бывает в сцене
    o.emission = float4(0.0, 0.0, 0.0, 0.0);
    // Ноль = «экранный AO этот пиксель не затеняет» (та же конвенция, что у неба и фракталов).
    o.ambient  = float4(0.0, 0.0, 0.0, 0.0);
    return o;
}
