#include "sparse_rank.hlsli"

struct VSInput
{
    float3 a_pos     : POSITION;
    float2 a_uv      : TEXCOORD0;
    uint   instanceID : SV_InstanceID;
};

struct VSOutput
{
    float4 position                           : SV_Position;
    [[vk::location(0)]] float2 v_uv           : TEXCOORD0;
    [[vk::location(1)]] float  v_alpha        : TEXCOORD1;
    [[vk::location(2)]] nointerpolation uint v_row : TEXCOORD2;   // row → текст-канал в FS
    float4 clip                               : SV_ClipDistance0;
};

StructuredBuffer<float4x4> ModelMatrixBlock : register(t0, space0);
StructuredBuffer<int>      Rows           : register(t1, space0);   // -1 = нет строки
struct InstanceData { float alpha; uint flags; };
StructuredBuffer<InstanceData> InstanceDataBlock : register(t2, space0);
SPARSE_CHANNEL(Clip, t3, space0)                                     // → ClipWords, ClipRank(row)
StructuredBuffer<uint>   ClipIndex : register(t4, space0);
StructuredBuffer<float4> ClipRect  : register(t5, space0);           // left, top, right, bottom в NDC

//@push ui_camera
cbuffer UICamera : register(b0, space1) { float4 ui_camera; };

VSOutput main(VSInput input)
{
    VSOutput o;

    int row = Rows[input.instanceID];
    if (row < 0) {   // нет строки — уводим примитив целиком за клип
        o = (VSOutput)0;
        o.position = float4(2.0, 2.0, 2.0, 1.0);
        return o;
    }

    float4x4 m = ModelMatrixBlock[row];
    float3 p = mul((float3x3)m, input.a_pos);
    float  w = 1.0 - p.z * tan(0.5 * ui_camera.x);
    o.position = float4(p.xy + float2(m[0][3], m[1][3]) * w, m[2][3] * w, w);

    // Расстояния до сторон рамки считаются до деления на w: тогда срез ровный и под перспективой.
    o.clip = float4(1.0, 1.0, 1.0, 1.0);
    const int clip_rank = ClipRank(row);
    if (clip_rank >= 0) {
        const float4 rect = ClipRect[ClipIndex[clip_rank]];
        o.clip = float4(o.position.x - rect.x * o.position.w, rect.z * o.position.w - o.position.x,
                        rect.y * o.position.w - o.position.y, o.position.y - rect.w * o.position.w);
    }
    // Квад теперь в КАНОНЕ развёртки (v-down, top-left origin — как glyph-атлас и albedo). UV идёт
    // напрямую, без флипа: раньше квад был v-up и здесь стоял `1.0 - a_uv.y`; после канона это стало
    // двойным флипом → текст/текстуры вверх ногами. См. канон в main_pass.vert (cross(T,N)).
    o.v_uv     = input.a_uv;
    o.v_alpha  = InstanceDataBlock[row].alpha;
    o.v_row    = (uint)row;
    return o;
}
