// 2D-фрактал: множество Мандельброта во ВСЮ видимую область, без зависимости от поворота
// камеры — только от её ПОЗИЦИИ. Пан — x/y камеры, зум — экспонента от z (октава на юнит).
// Экранная точка приходит готовым NDC из fractal2d.vert.hlsl; аспект — из proj.
//
// ГЛУБИНА — ПЕРЕТУРБАЦИЯ: CPU раз в кадр итерирует РЕФЕРЕНС (центр окна) в double и заливает
// орбиту Z_0..Z_{len-1} буфером (DefaultResources, FRACTAL_ORBIT_BUFFER). Пиксель итерирует
// только ДЕЛЬТУ от референса: для z = Z + dz алгебраически ТОЧНО dz' = (2Z + dz)·dz + dc,
// где dc = смещение пикселя от центра — оно мало́ по построению и живёт в обычном float.
// Абсолютная координата пикселя нигде не материализуется → нет и потери её младших битов
// (прежний шум float с ~17 октав, потом df до ~45). Предел резкости теперь — double
// референса (~50 октав); наводка пана — ulp float-позиции камеры (~24 октавы; дальше центр
// квантуется — это сдвиг, не шум). Безлимит — big-float референса на CPU, шейдер не меняется.
//
// РЕ-БАЗИРОВАНИЕ (Чжуоран): если |z| < |dz| — точка ближе к началу орбиты (Z_0 = 0), чем к
// текущему референсу: δ перепривязывается (dz = z, m = 0). Это же закрывает классические
// «глитчи» перетурбации и разворот на конце короткой орбиты (референс сбежал).
//
// Сэмплеры — ГЛОБАЛКИ MAIN_PASS (биндит пасс, не материал): слот 0 — тень, слот 1 — env.
// Фракталу не нужны, но объявлены и «заякорены» веткой в main(): DXC стрипает неиспользуемые
// ресурсы, а SDL GPU ждёт ПЛОТНЫЕ слоты от 0 — см. тот же приём в skybox.frag.hlsl.

[[vk::combinedImageSampler]]
Texture2DArray<float>     u_shadowDepthArray  : register(t0, space2);
[[vk::combinedImageSampler]]
SamplerComparisonState    u_shadowSampler     : register(s0, space2);

[[vk::combinedImageSampler]]
TextureCube  u_envCube    : register(t1, space2);
[[vk::combinedImageSampler]]
SamplerState u_envSampler : register(s1, space2);

// Камера — fs-буфер t2 (после 2 сэмплеров пасса): отсюда только зум (позиция z из
// rigid-inverse view) и аспект (диагональ proj). Пан x/y камеры сюда НЕ читается — он
// уже запечён в референс-орбиту на CPU (из того же состояния камеры этого слота).
struct CameraData
{
    float4x4 view;
    float4x4 proj;
};
StructuredBuffer<CameraData> Camera : register(t2, space2);

// Референс-орбита: [0] — заголовок (len, asuint в .x), [1 + n] — Z_n. Раскладку пишет
// апдейтер FRACTAL_ORBIT_BUFFER (DefaultResources.cpp).
StructuredBuffer<float2> Orbit : register(t3, space2);

// Push-константы _Fractal (fragment слот 0). Раскладка = FractalPushData (DefaultResources.h).
cbuffer FractalParams : register(b0, space3)
{
    float time;       // анимация палитры
    uint  max_iter;   // потолок итераций (адаптив ниже поднимается к нему с зумом)
    float _pad0;
    float _pad1;
};

struct PSInput
{
    float4 sv_pos : SV_Position;
    [[vk::location(0)]] float2 v_ndc : TEXCOORD0;
};

struct PSOutput
{
    float4 color    : SV_Target0;   // линейный HDR-цвет сцены
    float4 emission : SV_Target1;   // MRT пасса: яркая кромка множества слегка блумит
};

// Зум: полу-высота видимой области = HALF_H0 · 2^z — на стартовой камере игры (z=3.5)
// видно всё множество, полёт вперёд удваивает увеличение каждый юнит. Маппинг пана
// (x/y·0.25 − 0.6) живёт на CPU (центр референса) — здесь его нет.
static const float HALF_H0 = 0.124;

// Палитра IQ-косинусами: гладкая, циклится по дробному счётчику побега.
float3 Palette(float t)
{
    return 0.5 + 0.5 * cos(6.28318530718 * (t + float3(0.0, 0.33, 0.67)));
}

PSOutput main(PSInput input)
{
    PSOutput o;

    // Зум из позиции камеры (rigid-inverse view, как в main_pass.frag), аспект из proj
    // (m00 = 1/(aspect·tan), m11 = 1/tan → m11/m00 = aspect; диагональ инвариантна к
    // конвенции хранения).
    float4x4 view = Camera[0].view;
    float3 camPos = -mul(transpose((float3x3)view), view._m03_m13_m23);
    float aspect  = Camera[0].proj._m11 / Camera[0].proj._m00;

    float half_h = HALF_H0 * exp2(camPos.z);
    // Смещение пикселя от центра окна = от референса. Только оно и нужно: мало́ → float.
    float2 dc = input.v_ndc * float2(half_h * aspect, half_h);

    // Детализация от зума: +96 итераций на октаву (dwell у границы растёт быстро),
    // потолок — max_iter из push.
    float octaves = max(0.0, -log2(half_h));
    uint  it      = min(max_iter, 64u + (uint)(96.0 * octaves));

    uint len = asuint(Orbit[0].x);   // фактическая длина орбиты (референс мог сбежать)

    float3 col = float3(0.0, 0.0, 0.0);   // дефолт: внутренность множества / нет орбиты
    float3 emi = float3(0.0, 0.0, 0.0);

    if (len >= 2)
    {
        float2 dz = float2(0.0, 0.0);
        float  m2 = 0.0;
        uint   m  = 0;   // индекс в орбите; НЕ равен i — ре-базирование сбрасывает m в 0
        uint   i  = 0;
        for (; i < it; ++i) {
            // dz' = (2Z + dz)·dz + dc (комплексно) — точная алгебра, без приближений.
            float2 Z = Orbit[1 + m];
            float2 t = float2(2.0 * Z.x + dz.x, 2.0 * Z.y + dz.y);
            dz = float2(t.x * dz.x - t.y * dz.y, t.x * dz.y + t.y * dz.x) + dc;
            ++m;

            float2 z = Orbit[1 + m] + dz;   // полное значение — только для тестов/окраски
            m2 = dot(z, z);
            if (m2 > 256.0) break;

            // Ре-базирование: точка ближе к Z_0=0, чем к референсу, ИЛИ орбита кончилась.
            if (m2 < dot(dz, dz) || m >= len - 1) { dz = z; m = 0; }
        }

        if (i < it) {
            // Снаружи: гладкий (дробный) счётчик побега → палитра. Быстрый побег (далеко
            // от множества) — темно, у границы (большой sn) — яркие полосы.
            float sn = (float)i + 1.0 - log2(0.5 * log2(m2));
            col = Palette(sn * 0.02 + time * 0.02) * saturate(sn * 0.08);
            emi = col * col * 0.6;   // квадрат: блумят только яркие полосы кромки
        }
    }

    o.color    = float4(col, 1.0);
    o.emission = float4(emi, 0.0);

    // ЯКОРЬ t0/t1: ветка никогда не выполняется (sv_pos.x >= 0 во вьюпорте), но компилятор
    // доказать этого не может → глобалки пасса не стрипаются, слоты остаются плотными.
    if (input.sv_pos.x < -1.0) {
        o.color.r += u_shadowDepthArray.SampleCmpLevelZero(u_shadowSampler, float3(0.0, 0.0, 0.0), 0.0);
        o.color.g += u_envCube.Sample(u_envSampler, float3(0.0, 1.0, 0.0)).g;
    }

    return o;
}
