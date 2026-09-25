#include "main_pass/vertex_common.hlsl"

VSOutput main(VSInput input)
{
    int row = Rows[input.instanceID];   // строка трансформа = строка инстанс-данных
    if (row < 0) return CulledVertex();

    float4x4 modelMatrix = ModelMatrixBlock[row];
    float4 worldPos = mul(modelMatrix, float4(input.a_pos, 1.0));

    // В HLSL mul(M, v) — столбцовое умножение
    float3x3 normalMatrix = (float3x3)modelMatrix;
    float3 worldNormal    = normalize(mul(normalMatrix, input.a_normal));
    float3 worldTangent   = normalize(mul(normalMatrix, input.a_tangent));
    // КАНОН развёртки: top-left текстура → V-down → UV левосторонняя относительно нормали. Битангенс
    // = cross(T,N) (а НЕ cross(N,T)): даёт B вдоль +V, чтобы зелёный нормалки и ось V у POM совпали с
    // v-down развёрткой (quad/sphere). Единый глобальный знак — без per-material флагов.
    float3 worldBitangent = normalize(cross(worldTangent, worldNormal));

    return FinishVertex(input, row, worldPos.xyz, worldNormal, worldTangent, worldBitangent);
}
