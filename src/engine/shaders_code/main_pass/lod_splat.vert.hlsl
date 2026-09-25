// Объект одной точкой в пиксель: модель из одной вершины, топология — точки. Точку закрашивает
// фрагментник материала; нормаль смотрит на камеру.

#define VERTEX_POINT_SIZE
#include "main_pass/vertex_common.hlsl"

VSOutput main(VSInput input)
{
    int row = Rows[input.instanceID];
    if (row < 0) return CulledVertex();

    float4x4 view = Camera[0].view;
    float3 worldPos = mul(ModelMatrixBlock[row], float4(input.a_pos, 1.0)).xyz;
    float3 right  = view[0].xyz;
    float3 toward = view[2].xyz;

    return FinishVertex(input, row, worldPos, toward, right, normalize(cross(right, toward)));
}
