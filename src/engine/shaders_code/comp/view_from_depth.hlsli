#ifndef VIEW_FROM_DEPTH_HLSLI
#define VIEW_FROM_DEPTH_HLSLI

// Обратное преобразование «буфер глубины → view-пространство»: общее для экранных compute-эффектов
// (AO, туман). Вынесено в отдельный файл не ради строк, а потому что вывод неочевиден и разъезд
// копий не сигналит: движок отдаёт glm-проекцию с диапазоном NDC z ∈ [-1,1]
// (GLM_CLIP_CONTROL_RH_NO — GLM_FORCE_DEPTH_ZERO_TO_ONE нигде не задан), и формула «как в ZO» дала
// бы правдоподобную, но неверную глубину. Здесь конвенция диапазона не участвует вовсе: A и B
// берутся из СТОЛБЦОВ proj, и она уже сидит в них.
//
// NDC y — ВВЕРХ (UI_Yoga::px→NDC: `T = 1 - y/H*2`), поэтому uv→ndc идёт с флипом по y.
//
// Требует объявленным ДО включения: StructuredBuffer<CameraData> Camera с полями view/proj
// (как sparse_rank.hlsli у материалов — файл описывает тело, биндинги остаются за шейдером).

// Глубина буфера → view-z (отрицательный, камера смотрит вдоль -z). Инверсия d = z_c/w_c при
// z_c = A·z_v + B, w_c = -z_v.
float ViewZ(float d)
{
    const float A = Camera[0].proj[2][2];
    const float B = Camera[0].proj[2][3];
    return -B / (A + d);
}

// uv кадра + глубина → точка во view-пространстве.
float3 ViewPos(float2 uv, float d)
{
    const float z = ViewZ(d);
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    // x_v = -ndc.x·z / proj[0][0] (симметричная перспектива: сдвиговых членов нет)
    return float3(-ndc.x * z / Camera[0].proj[0][0],
                  -ndc.y * z / Camera[0].proj[1][1],
                  z);
}

#endif
