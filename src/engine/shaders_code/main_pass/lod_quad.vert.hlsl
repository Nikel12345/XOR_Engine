// Плоская модель (в плоскости XY) всегда лицом к точке камеры: от трансформа берутся перенос
// и средний по трём осям масштаб.
// Выход совпадает с main_pass.vert — фрагментник общий.

struct VSInput
{
    float3 a_pos     : POSITION;
    float2 a_uv      : TEXCOORD0;
    float3 a_normal  : NORMAL;
    float3 a_tangent : TANGENT;
    uint instanceID  : SV_InstanceID;
};

struct VSOutput
{
    float4 position         : SV_Position;
    float2 v_uv             : TEXCOORD0;
    float3 v_worldPos       : TEXCOORD1;
    float3 v_worldNormal    : TEXCOORD2;
    float3 v_worldTangent   : TEXCOORD3;
    float3 v_worldBitangent : TEXCOORD4;
    float  v_alpha          : TEXCOORD5;
    nointerpolation int v_row : TEXCOORD6;
};

StructuredBuffer<float4x4> ModelMatrixBlock : register(t0, space0);
StructuredBuffer<int>      Rows             : register(t1, space0);

struct CameraData
{
    float4x4 view;
    float4x4 proj;
};
StructuredBuffer<CameraData> Camera : register(t2, space0);

struct InstanceData { float alpha; uint flags; };
StructuredBuffer<InstanceData> InstanceDataBlock : register(t3, space0);

VSOutput main(VSInput input)
{
    VSOutput output;

    int row = Rows[input.instanceID];
    if (row < 0) {
        output = (VSOutput)0;
        output.position = float4(2.0, 2.0, 2.0, 1.0);
        output.v_row = -1;
        return output;
    }

    float4x4 view = Camera[0].view;
    float4x4 proj = Camera[0].proj;

    float4x4 m = ModelMatrixBlock[row];
    float3 origin = float3(m[0][3], m[1][3], m[2][3]);
    float  scale  = (length(float3(m[0][0], m[1][0], m[2][0])) +
                     length(float3(m[0][1], m[1][1], m[2][1])) +
                     length(float3(m[0][2], m[1][2], m[2][2]))) / 3.0;

    // На точку камеры, а не параллельно экрану: у соседних плоскостей разный наклон, и равенство
    // глубин при перекрытии сходится в линию, а не в площадь (порядок инстансов от кадра к кадру свой).
    float3 camPos = -(view[0].xyz * view[0].w + view[1].xyz * view[1].w + view[2].xyz * view[2].w);
    float3 toward = normalize(camPos - origin);
    float3 right  = normalize(cross(view[1].xyz, toward));
    float3 up     = cross(toward, right);
    float4 worldPos = float4(origin + (right * input.a_pos.x + up * input.a_pos.y) * scale, 1.0);

    output.position         = mul(proj, mul(view, worldPos));
    output.v_worldPos       = worldPos.xyz;
    output.v_worldNormal    = toward;
    output.v_uv             = input.a_uv;
    output.v_worldTangent   = right;
    output.v_worldBitangent = normalize(cross(right, toward));
    output.v_alpha          = InstanceDataBlock[row].alpha;
    output.v_row            = row;

    return output;
}
