// Тень lod_quad.vert: плоская модель лицом к световой камере, от трансформа перенос и средний масштаб.

#include "shadow_pass/vertex_common.hlsl"

VSOutput main(VSInput input)
{
    int row = Rows[input.instanceID];
    if (row < 0) return CulledShadowVertex();

    float4x4 m = ModelMatrixBlock[row];
    float4x4 view = LightCameras[currentCameraIndex].view;
    float3 worldPos = TransformOrigin(m) + (view[0].xyz * input.a_pos.x + view[1].xyz * input.a_pos.y) * TransformMeanScale(m);

    return FinishShadowVertex(worldPos);
}
