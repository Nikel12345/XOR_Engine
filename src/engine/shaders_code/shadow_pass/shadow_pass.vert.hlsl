struct VSInput {
    float3 a_pos     : POSITION;
    uint   instanceID : SV_InstanceID;
};


StructuredBuffer<float4x4> ModelMatrixBlock    : register(t0, space0);
// out_pib (выход GPU-каллинга): блоками по камерам. Блок световой камеры j лежит по
// СТРОГО известному смещению (1 + j) * num_instances — первый блок (1 размер PIB)
// всегда занят камерой игрока, даже если теневых камер нет. -1 = не виден камерой.
StructuredBuffer<int>      OutPib              : register(t1, space0);

struct LightCamera {
    float4x4 view;
    float4x4 proj;
};
StructuredBuffer<LightCamera> LightCameras : register(t2, space0);

// Раскладка = ShadowPushData (DefaultRenderPassSet.h), пушится на каждую световую камеру.
cbuffer CurrentCameraUBO : register(b0, space1) {
    int   currentCameraIndex;
    float currentFarRange;
    uint  is_ortho;         // здесь не используется (нужен фрагменту) — для совпадения раскладки
    uint  num_instances;    // размер одного камерного блока out_pib (= число PIB-записей)
};

struct VSOutput {
    float4 sv_pos    : SV_Position;
    float3 viewPosWS : TEXCOORD0;   // view-space позиция, интерполируется линейно
};


VSOutput main(VSInput input)
{
    VSOutput o;
    int row = OutPib[(1 + currentCameraIndex) * num_instances + input.instanceID];
    if (row < 0) {
        // Инстанс не виден ЭТОЙ световой камерой → вырожденная позиция (клипается целиком).
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