#ifndef UI_NINE_SLICE_FRAG_HLSL
#define UI_NINE_SLICE_FRAG_HLSL

float4 main(PSInput input) : SV_Target0
{
    float4 color = getColor(input);
    color.a *= input.v_alpha;

    // Якорь слота 0: DXC выбрасывает неиспользуемый ресурс, а SDL GPU считает сэмплеры подряд с нуля,
    // и без слота 0 шейдер остался бы без albedo. Ветка не выполняется, но компилятор этого не докажет.
    if (input.sv_pos.x < -1.0)
        color += u_glyph.SampleLevel(u_glyphSampler, float3(0.0, 0.0, 0.0), 0.0);

    // depth_write включён: прозрачный пиксель записал бы глубину и закрыл то, что под ним.
    clip(color.a - 0.004);
    return color;
}

#endif
