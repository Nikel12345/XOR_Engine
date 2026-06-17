// Сплошной цвет рамки. Цвет приходит push-константой (fragment slot 0 → b0, space3),
// см. DefaultRenderPassNamespace::DebugColliderPushData.
cbuffer DebugColorBlock : register(b0, space3) {
    float4 u_color;
};

float4 main(float4 sv_pos : SV_Position) : SV_Target0
{
    return u_color;
}
