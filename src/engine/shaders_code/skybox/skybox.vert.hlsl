// Скайбокс: куб [-1,1] вокруг камеры. Энтити TRANSFORMLESS (без Positions): PIB=-1 →
// каллинг скаттерит безусловно, out_pib/трансформы/инстанс-данные здесь НЕ читаются —
// позиция строится целиком из камерного буфера. Вход — только POSITION (pull VS).

struct VSInput
{
    float3 a_pos : POSITION;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 v_dir    : TEXCOORD0;   // мировое направление взгляда = угол куба (куб осестабилен)
};

// Тот же элемент, что в main_pass.vert; [0] — камера игрока.
struct CameraData
{
    float4x4 view;
    float4x4 proj;
};
StructuredBuffer<CameraData> Camera : register(t0, space0);

VSOutput main(VSInput input)
{
    VSOutput output;

    // (float3x3) отрезает 4-й столбец view (трансляцию) → куб всегда центрирован на камере,
    // остаётся чистое вращение (тот же приём, что normalMatrix в main_pass.vert).
    float3 dirView = mul((float3x3)Camera[0].view, input.a_pos);
    float4 clip    = mul(Camera[0].proj, float4(dirView, 1.0));

    output.position = clip.xyww;   // z=w → NDC-глубина ровно 1.0: проходит LESS_OR_EQUAL только по клиру
    output.v_dir    = input.a_pos;
    return output;
}
