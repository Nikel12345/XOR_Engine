#ifndef SHADOW_VERTEX_COMMON_HLSL
#define SHADOW_VERTEX_COMMON_HLSL

// Общее у вершинников теневого прохода: вход, выход, буферы, отброс инстанса без строки и
// перевод мировой позиции в световую камеру. Свой у каждого вершинника только мировая позиция.

#include "main_pass/math.hlsl"

struct VSInput {
    float3 a_pos      : POSITION;
    uint   instanceID : SV_InstanceID;
};

StructuredBuffer<float4x4> ModelMatrixBlock : register(t0, space0);
// SV_InstanceID = first_instance + i, а first_instance команды указывает на её записи в Rows.
StructuredBuffer<int>      Rows             : register(t1, space0);

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

// Строки нет → вырожденная позиция (клипается целиком).
VSOutput CulledShadowVertex()
{
    VSOutput o = (VSOutput)0;
    o.sv_pos = float4(2.0, 2.0, 2.0, 1.0);
    return o;
}

VSOutput FinishShadowVertex(float3 worldPos)
{
    VSOutput o;
    float4 viewPos = mul(LightCameras[currentCameraIndex].view, float4(worldPos, 1.0));
    o.sv_pos    = mul(LightCameras[currentCameraIndex].proj, viewPos);
    o.viewPosWS = viewPos.xyz;
    return o;
}

#endif
