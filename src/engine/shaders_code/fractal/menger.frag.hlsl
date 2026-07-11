// 3D-фрактал: губка Менгера рэймарчем в ПЕРЕНОРМИРОВАННОМ КАДРЕ — «4-я координата» позиции
// (масштаб) вынесена из float-координат: CPU держит стек символов спуска (целая глубина) и
// локальную позицию камеры в кадре текущей ячейки (всегда O(1)); ребейз ×3/÷3 делает апдейтер
// кадра (DefaultResources.cpp). Шейдер всегда работает при масштабе ~1 → предел точности float
// исчезает, а видимых уровней детализации — константа на ЛЮБОЙ глубине (бесконечный зум за
// константную цену — точное самоподобие губки).
//
// Геометрия в локальном кадре: свои уровни (текущая ячейка и мельче) — периодический
// IFS-фолдинг, как у корневой формулы (самоподобие: офсеты спуска кратны 2·3^k и mod-2
// фолдингом сокращаются); уровни ПРЕДКОВ (кресты, прорезающие окрестность, и внешний бокс) —
// K офсетов из буфера кадра, дальше их прячет туман. Скайбокс-путь прежний: куб вокруг камеры,
// _skybox_vs, v_dir — направление луча (ротация ребейзом не меняется), глубина ровно 1.0.
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

// Кадр фрактала (пишет апдейтер FRACTAL_FRAME_BUFFER): [0] = (позиция камеры в локальном
// кадре, глубина), [1] = (K, 0,0,0), [2+j] = (офсет предка j+1 уровней вверх, 3^(j+1)).
// Камерный буфер здесь НЕ объявлен намеренно: origin луча идёт отсюда (пост-ребейз),
// ротация уходит в v_dir ещё в VS — а неиспользуемое объявление DXC стрипает, и слоты
// storage-буферов становятся дырявыми (SDL ждёт плотные) → пайплайн не собирается.
StructuredBuffer<float4> Frame : register(t2, space2);

// Push-константы _Fractal (fragment слот 0). Раскладка = FractalPushData (DefaultResources.h).
cbuffer FractalParams : register(b0, space3)
{
    float time;        // медленное вращение источника света
    uint  max_steps;   // потолок шагов рэймарча
    float _pad0;
    float _pad1;
};

struct PSInput
{
    float4 sv_pos : SV_Position;
    [[vk::location(0)]] float3 v_dir : TEXCOORD0;
};

struct PSOutput
{
    float4 color    : SV_Target0;   // линейный HDR-цвет сцены
    float4 emission : SV_Target1;   // MRT пасса: у губки эмиссии нет
};

static const float K_PX       = 8e-4;   // угловой размер пикселя (~1080p@45°): eps(t) = t·K_PX
static const int   MAX_LEVELS = 12;     // потолок СВОИХ уровней: мельче не видно на любом t

// GLSL-мод (floor-периодический): HLSL fmod знаковый и для фолдинга не годится.
float3 glmod(float3 x, float y) { return x - y * floor(x / y); }

float sdBox(float3 p, float3 b)
{
    float3 q = abs(p) - b;
    return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
}

// Крест 27-разбиения ячейки [-1,1] в её юнитах (= уровень m=0 цикла ниже; делитель 3 как в IQ).
// Зеркалится в MengerCrossDE (DefaultResources.cpp) — критерий ребейза считает то же самое.
float MengerCross(float3 q)
{
    float3 a = glmod(q, 2.0) - 1.0;
    float3 r = abs(1.0 - 3.0 * abs(a));
    float da = max(r.x, r.y);
    float db = max(r.y, r.z);
    float dc = max(r.z, r.x);
    return (min(da, min(db, dc)) - 1.0) / 3.0;
}

// DE в локальном кадре: бокс K уровней вверх (K=0 → корень) + кресты предков (офсеты из
// кадра) + свои уровни периодическим фолдингом (levels — адаптив от дистанции).
float MengerDE(float3 p, int levels, uint K)
{
    // [loop]: DXC инлайнит MengerDE в 5 мест (марш + 4 тапа нормали) — без атрибута
    // анроллинг runtime-циклов раздувает SPIR-V до отказа пайплайн-компиляции драйвера.
    float4 fb = float4(0.0, 0.0, 0.0, 1.0);
    if (K > 0u) fb = Frame[1 + K];
    float d = sdBox(p / fb.w + fb.xyz, float3(1.0, 1.0, 1.0)) * fb.w;
    [loop] for (uint j = 0; j < K; ++j) {
        float4 f = Frame[2 + j];
        d = max(d, MengerCross(p / f.w + f.xyz) * f.w);
    }

    float s = 1.0;
    [loop] for (int m = 0; m < levels; ++m) {
        float3 a = glmod(p * s, 2.0) - 1.0;
        s *= 3.0;
        float3 r = abs(1.0 - 3.0 * abs(a));
        float da = max(r.x, r.y);
        float db = max(r.y, r.z);
        float dc = max(r.z, r.x);
        d = max(d, (min(da, min(db, dc)) - 1.0) / s);
    }
    return d;
}

// Сколько СВОИХ уровней раскрывать на дистанции t (в локальных юнитах): деталь мельче
// пиксельного футпринта не видна — не платим. 1/log2(3) ≈ 0.63093.
int LevelsFor(float t)
{
    float need = log2(1.0 / max(t * K_PX, 1e-12)) * 0.63093;
    return clamp((int)need + 1, 1, MAX_LEVELS);
}

// Палитра для лёгкой вариации альбедо по высоте локального кадра.
PSOutput main(PSInput input)
{
    PSOutput o;

    float3 ro = Frame[0].xyz;              // позиция камеры в локальном кадре (пост-ребейз)
    uint   K  = (uint)Frame[1].x;
    float3 rd = normalize(input.v_dir);    // ротация из view (VS) — ребейз её не трогает

    float3 sky = lerp(float3(0.05, 0.06, 0.12), float3(0.010, 0.012, 0.030),
                      saturate(rd.y * 2.0 + 0.2));
    float3 col = sky;

    float t     = 0.0;
    float tFar  = 6000.0;   // за туманом (см. lerp ниже); большие полости шагаются быстро —
                            // DE в пустоте растёт, шаги геометрические
    bool  culled = false;

    // На корневом уровне камера обычно СНАРУЖИ — слаб-тест бокса отрезает пустой подлёт и
    // ранний промах. Внутри (K>0) march стартует с нуля — стены рядом по построению кадра.
    if (K == 0u) {
        float3 rds = sign(rd) * max(abs(rd), 1e-9);
        float3 inv = 1.0 / rds;
        float3 lo  = (-1.0 - ro) * inv;
        float3 hi  = ( 1.0 - ro) * inv;
        float3 tn3 = min(lo, hi);
        float3 tf3 = max(lo, hi);
        float tEnter = max(max(tn3.x, tn3.y), tn3.z);
        float tExit  = min(min(tf3.x, tf3.y), tf3.z);
        if (tExit < max(tEnter, 0.0)) culled = true;
        else { t = max(tEnter, 0.0); tFar = tExit + 1e-3; }
    }

    if (!culled)
    {
        bool hit = false;
        uint steps = 0;
        [loop] for (; steps < max_steps; ++steps)
        {
            float3 pos = ro + rd * t;
            float  d   = MengerDE(pos, LevelsFor(t), K);
            float  eps = max(t * K_PX, 1e-7);
            if (d < eps) { hit = true; break; }
            t += d;
            if (t > tFar) break;
        }

        if (hit)
        {
            // Нормаль — тетраэдральный градиент DE на масштабе eps попадания.
            float h = max(t * K_PX, 2e-6);
            int   lv = LevelsFor(t);
            float2 e = float2(1.0, -1.0) * 0.57735;
            float3 n = normalize(
                e.xyy * MengerDE(ro + rd * t + e.xyy * h, lv, K) +
                e.yyx * MengerDE(ro + rd * t + e.yyx * h, lv, K) +
                e.yxy * MengerDE(ro + rd * t + e.yxy * h, lv, K) +
                e.xxx * MengerDE(ro + rd * t + e.xxx * h, lv, K));

            // Ключевой свет медленно кружит (time из push) + небо сверху; AO — из числа
            // шагов марша (щели и углы «доходят» дольше → темнее).
            float3 L    = normalize(float3(cos(time * 0.1), 0.75, sin(time * 0.1)));
            float  diff = max(dot(n, L), 0.0);
            float  amb  = 0.22 + 0.18 * n.y;
            float  ao   = pow(saturate(1.0 - (float)steps / (float)max_steps), 1.5);

            // Альбедо ТОЛЬКО от нормали (масштабно-инвариантно): привязка к локальной
            // позиции прыгала бы гаммой при каждом ребейзе (координаты кадра меняются,
            // мировая точка — нет). Верхние грани — тёплые, боковые/нижние — холодные.
            float3 albedo = lerp(float3(0.40, 0.44, 0.52),
                                 float3(0.90, 0.72, 0.48),
                                 saturate(n.y * 0.5 + 0.5));

            col = albedo * (amb + 0.9 * diff) * ao;
            // Туман в локальных юнитах, но с дальностью ПОД ВЕСЬ K-контекст (3^8 ≈ 6561):
            // при ручном масштабе камера может висеть в крупной полости глубокого кадра —
            // стены за тысячи локальных юнитов должны быть видны, а не съедены. Обрез
            // контекста за K уровней туман по-прежнему прячет.
            col = lerp(col, sky, saturate(t * (1.0 / 2500.0)));
        }
    }

    o.color    = float4(col, 1.0);
    o.emission = float4(0.0, 0.0, 0.0, 0.0);

    // ЯКОРЬ t0/t1: ветка никогда не выполняется (sv_pos.x >= 0 во вьюпорте), но компилятор
    // доказать этого не может → глобалки пасса не стрипаются, слоты остаются плотными.
    if (input.sv_pos.x < -1.0) {
        o.color.r += u_shadowDepthArray.SampleCmpLevelZero(u_shadowSampler, float3(0.0, 0.0, 0.0), 0.0);
        o.color.g += u_envCube.Sample(u_envSampler, float3(0.0, 1.0, 0.0)).g;
    }

    return o;
}
