#ifndef VERTEX_COMMON_HLSL
#define VERTEX_COMMON_HLSL

// Общее у вершинников основного прохода: входы, выход, буферы, отброс инстанса без строки и
// заполнение выхода. Свой у каждого вершинника только расчёт мировой позиции и базиса.
// VERTEX_POINT_SIZE перед включением — выход с размером точки (топология точек).

#include "main_pass/math.hlsl"

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
    // Строка трансформа этого инстанса (-1 = строки нет). Нужна фрагментнику, чтобы
    // прочитать префикс состояний вариантов; сам буфер вершинник НЕ читает — иначе его обязана
    // была бы биндить КАЖДАЯ sp с этим вершинником, включая чужие (теневые, фрактальные игровые).
    // nointerpolation: это индекс, а не величина.
    // Член ОБЯЗАН быть и в PSInput всех трёх прологов (main/transparent/untextured): вершинник
    // общий, разъехавшийся PSInput = молча битые локейшены.
    nointerpolation int v_row : TEXCOORD6;
#ifdef VERTEX_POINT_SIZE
    // Vulkan размер точки не подразумевает: без записи он не определён.
    [[vk::builtin("PointSize")]] float psize : PSIZE;
#endif
};

StructuredBuffer<float4x4> ModelMatrixBlock : register(t0, space0);
// SV_InstanceID = first_instance + i, а first_instance команды указывает на её записи в Rows.
StructuredBuffer<int>      Rows             : register(t1, space0);

struct CameraData
{
    float4x4 view;
    float4x4 proj;
};
StructuredBuffer<CameraData> Camera : register(t2, space0);

// Per-instance данные (alpha/flags). Индексируются ТЕМ ЖЕ row, что и матрица.
// Дублирует struct InstanceData в BaseComponents.h.
struct InstanceData { float alpha; uint flags; };
StructuredBuffer<InstanceData> InstanceDataBlock : register(t3, space0);

// Инстанс без строки: все вершины за одну clip-плоскость, примитив клипается целиком. Голый
// return нельзя — SV_Position был бы UB.
VSOutput CulledVertex()
{
    VSOutput output = (VSOutput)0;
    output.position = float4(2.0, 2.0, 2.0, 1.0);
    output.v_row = -1;
#ifdef VERTEX_POINT_SIZE
    output.psize = 1.0;
#endif
    return output;
}

VSOutput FinishVertex(VSInput input, int row, float3 worldPos, float3 normal, float3 tangent, float3 bitangent)
{
    VSOutput output;
    output.position         = mul(Camera[0].proj, mul(Camera[0].view, float4(worldPos, 1.0)));
    output.v_worldPos       = worldPos;
    output.v_worldNormal    = normal;
    output.v_uv             = input.a_uv;
    output.v_worldTangent   = tangent;
    output.v_worldBitangent = bitangent;
    output.v_alpha          = InstanceDataBlock[row].alpha;
    output.v_row            = row;
#ifdef VERTEX_POINT_SIZE
    output.psize = 1.0;
#endif
    return output;
}

#endif
