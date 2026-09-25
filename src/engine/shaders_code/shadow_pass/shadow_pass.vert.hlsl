#include "shadow_pass/vertex_common.hlsl"

VSOutput main(VSInput input)
{
    int row = Rows[input.instanceID];
    if (row < 0) return CulledShadowVertex();

    return FinishShadowVertex(mul(ModelMatrixBlock[row], float4(input.a_pos, 1.0)).xyz);
}
