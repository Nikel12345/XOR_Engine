// Тень lod_quad.vert: плоская модель лицом к световой камере, от трансформа перенос и средний масштаб.

struct VSInput {
    float3 a_pos      : POSITION;
    uint   instanceID : SV_InstanceID;
};

StructuredBuffer<float4x4> ModelMatrixBlock : register(t0, space0);
StructuredBuffer<int>      Rows             : register(t1, space0);

struct LightCamera {
    float4x4 view;
    float4x4 proj;
};
StructuredBuffer<LightCamera> LightCameras : register(t2, space0);

cbuffer CurrentCameraUBO : register(b0, space1) {
    int currentCameraIndex;
};

struct VSOutput {
    float4 sv_pos    : SV_Position;
    float3 viewPosWS : TEXCOORD0;
};

VSOutput main(VSInput input)
{
    VSOutput o;
    int row = Rows[input.instanceID];
    if (row < 0) {
        o = (VSOutput)0;
        o.sv_pos = float4(2.0, 2.0, 2.0, 1.0);
        return o;
    }

    float4x4 m = ModelMatrixBlock[row];
    float3 origin = float3(m[0][3], m[1][3], m[2][3]);
    float  scale  = (length(float3(m[0][0], m[1][0], m[2][0])) +
                     length(float3(m[0][1], m[1][1], m[2][1])) +
                     length(float3(m[0][2], m[1][2], m[2][2]))) / 3.0;

    float4x4 view = LightCameras[currentCameraIndex].view;
    float4 worldPos = float4(origin + (view[0].xyz * input.a_pos.x + view[1].xyz * input.a_pos.y) * scale, 1.0);
    float4 viewPos  = mul(view, worldPos);

    o.sv_pos    = mul(LightCameras[currentCameraIndex].proj, viewPos);
    o.viewPosWS = viewPos.xyz;
    return o;
}
