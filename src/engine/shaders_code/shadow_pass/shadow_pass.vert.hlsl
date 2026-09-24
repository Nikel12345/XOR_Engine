struct VSInput {
    float3 a_pos     : POSITION;
    uint   instanceID : SV_InstanceID;
};


StructuredBuffer<float4x4> ModelMatrixBlock    : register(t0, space0);
// SV_InstanceID = first_instance + i, а first_instance команды указывает на её записи в Rows.
StructuredBuffer<int>      Rows              : register(t1, space0);

struct LightCamera {
    float4x4 view;
    float4x4 proj;
};
StructuredBuffer<LightCamera> LightCameras : register(t2, space0);

// Вершинная инструкция ShadowCaster отдаёт из состояния прохода только номер камеры; остальное
// из ShadowPushData (far, is_ortho) забирает своим пушем фрагментник.
cbuffer CurrentCameraUBO : register(b0, space1) {
    int currentCameraIndex;
};

struct VSOutput {
    float4 sv_pos    : SV_Position;
    float3 viewPosWS : TEXCOORD0;   // view-space позиция, интерполируется линейно
};


VSOutput main(VSInput input)
{
    VSOutput o;
    int row = Rows[input.instanceID];
    if (row < 0) {
        // Строки нет → вырожденная позиция (клипается целиком).
        o = (VSOutput)0;
        o.sv_pos = float4(2.0, 2.0, 2.0, 1.0);
        return o;
    }
    float4x4 modelMatrix = ModelMatrixBlock[row];
    float4   worldPos    = mul(modelMatrix, float4(input.a_pos, 1.0));

    float4x4 view    = LightCameras[currentCameraIndex].view;
    float4   viewPos = mul(view, worldPos);

    o.sv_pos    = mul(LightCameras[currentCameraIndex].proj, viewPos);
    o.viewPosWS = viewPos.xyz;


    return o;
}