// No SV_Depth output — hardware projected depth is written automatically.
// This enables Early-Z: the GPU rejects occluded fragments before running
// any shader, critical for shadow passes over complex geometry.
void main(float3 viewPosWS : TEXCOORD0)
{
}