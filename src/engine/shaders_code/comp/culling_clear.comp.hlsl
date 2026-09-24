RWStructuredBuffer<uint> Counters : register(u0, space1);

cbuffer ClearParams : register(b0, space2) {
    uint total;
};

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x < total) Counters[tid.x] = 0u;
}
