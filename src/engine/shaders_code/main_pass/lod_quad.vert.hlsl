// Плоская модель (в плоскости XY) всегда лицом к точке камеры: от трансформа берутся перенос
// и средний по трём осям масштаб.

#include "main_pass/vertex_common.hlsl"

VSOutput main(VSInput input)
{
    int row = Rows[input.instanceID];
    if (row < 0) return CulledVertex();

    float4x4 view = Camera[0].view;
    float4x4 m = ModelMatrixBlock[row];
    float3 origin = TransformOrigin(m);

    // На точку камеры, а не параллельно экрану: у соседних плоскостей разный наклон, и равенство
    // глубин при перекрытии сходится в линию, а не в площадь (порядок инстансов от кадра к кадру свой).
    float3 camPos = -(view[0].xyz * view[0].w + view[1].xyz * view[1].w + view[2].xyz * view[2].w);
    float3 toward = normalize(camPos - origin);
    float3 right  = normalize(cross(view[1].xyz, toward));
    float3 up     = cross(toward, right);
    float3 worldPos = origin + (right * input.a_pos.x + up * input.a_pos.y) * TransformMeanScale(m);

    return FinishVertex(input, row, worldPos, toward, right, normalize(cross(right, toward)));
}
