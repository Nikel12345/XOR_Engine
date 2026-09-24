// Объект одной точкой в пиксель: модель из одной вершины, топология — точки. Выход совпадает с
// main_pass.vert, поэтому точку закрашивает фрагментник материала; нормаль смотрит на камеру.

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
    // Vulkan размер точки не подразумевает: без записи он не определён.
    [[vk::builtin("PointSize")]] float psize : PSIZE;
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
    output.psize = 1.0;

    int row = Rows[input.instanceID];
    if (row < 0) {
        output = (VSOutput)0;
        output.position = float4(2.0, 2.0, 2.0, 1.0);
        output.v_row = -1;
        output.psize = 1.0;
        return output;
    }

    float4x4 view = Camera[0].view;
    float4x4 proj = Camera[0].proj;
    float4 worldPos = mul(ModelMatrixBlock[row], float4(input.a_pos, 1.0));

    float3 right  = view[0].xyz;
    float3 toward = view[2].xyz;

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
