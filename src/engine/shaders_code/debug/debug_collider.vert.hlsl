// Дебаг-рамки коллайдеров. Тянет только POSITION единичной модели (куб [-1..1] /
// сфера r=1) и инстансит её матрицей энтити-формы. Те же storage-буферы, что у main:
// t0 = трансформы, t1 = индекс позиции, t2 = камера.
struct VSInput {
    float3 a_pos      : POSITION;
    uint   instanceID : SV_InstanceID;
};

struct VSOutput {
    float4 position : SV_Position;
};

StructuredBuffer<float4x4> ModelMatrixBlock    : register(t0, space0);
// out_pib, блок 0 (камера игрока) — см. main_pass.vert.hlsl. -1 = отсечён каллингом.
StructuredBuffer<int>      OutPib              : register(t1, space0);

struct CameraData { float4x4 view; float4x4 proj; };
StructuredBuffer<CameraData> Camera : register(t2, space0);

VSOutput main(VSInput input)
{
    VSOutput output;

    int row = OutPib[input.instanceID];
    if (row < 0) {
        output.position = float4(2.0, 2.0, 2.0, 1.0);   // клипается целиком
        return output;
    }

    float4x4 view = Camera[0].view;
    float4x4 proj = Camera[0].proj;

    float4x4 modelMatrix = ModelMatrixBlock[row];
    float4   worldPos    = mul(modelMatrix, float4(input.a_pos, 1.0));

    output.position = mul(proj, mul(view, worldPos));
    return output;
}
