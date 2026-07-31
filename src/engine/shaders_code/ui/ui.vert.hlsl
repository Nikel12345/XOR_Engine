// UI-вершинник: NDC-квад. Трансформ (Positions 4x4) = ЭКРАННАЯ матрица, дающая clip напрямую —
// view/proj НЕТ (UI не зависит от камеры мира). row из OutPib индексирует трансформ и инстанс-
// данные (как main_pass.vert), и передаётся во фрагментник для разреженного текст-канала.
// Модель — юнит-квад [0,1]²; матрица раскладывает его в нужный NDC-прямоугольник.

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
};

StructuredBuffer<float4x4> ModelMatrixBlock : register(t0, space0);
StructuredBuffer<int>      OutPib           : register(t1, space0);   // -1 = отсечён/нет строки
struct InstanceData { float alpha; uint flags; };
StructuredBuffer<InstanceData> InstanceDataBlock : register(t2, space0);

VSOutput main(VSInput input)
{
    VSOutput o;

    int row = OutPib[input.instanceID];
    if (row < 0) {   // нет строки/отсечён — уводим примитив целиком за клип
        o = (VSOutput)0;
        o.position = float4(2.0, 2.0, 2.0, 1.0);
        return o;
    }

    float4x4 m = ModelMatrixBlock[row];
    o.position = mul(m, float4(input.a_pos, 1.0));   // матрица даёт clip НАПРЯМУЮ (NDC), без view/proj
    // Квад теперь в КАНОНЕ развёртки (v-down, top-left origin — как glyph-атлас и albedo). UV идёт
    // напрямую, без флипа: раньше квад был v-up и здесь стоял `1.0 - a_uv.y`; после канона это стало
    // двойным флипом → текст/текстуры вверх ногами. См. канон в main_pass.vert (cross(T,N)).
    o.v_uv     = input.a_uv;
    o.v_alpha  = InstanceDataBlock[row].alpha;
    o.v_row    = (uint)row;
    return o;
}
