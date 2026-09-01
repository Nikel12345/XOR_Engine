#include "PCH.h"
#include "FractalShaderSet.h"
#include "FractalUpdateSet.h"
#include "EngineContext.h"
#include "ShaderManager.h"

// Единая точка регистрации СВОИХ push-констант — ОДИН РАЗ на инициализации (см. заголовок).
// Sp приезжают из shaders.json своей сцены; какая из них загружена сейчас — здесь не важно.
void FractalShaderSet::RegisterShaderFuncs(EngineContext* ctx)
{
    ShaderManager* sm = ctx->GetShaderManager();

    // Пуши движковых sp (ShadowCaster/Wireframe) регистрирует сам движок вместе с их
    // созданием — Engine::InitDefaultShaders. Игре остаются только её собственные.
    //
    // Фрактальные фоны: у каждой сцены свой sp, имена разные — регистрируем обе функции разом,
    // разбор имени сцены больше не нужен. Тело MAIN_PASS передаёт push_func nullptr вместо данных →
    // типизированная форма (разыменовывает raw) не годится; берём сырую, данные строим в лямбде
    // (fragment slot 0 → b0, space3).
    sm->CreatePushFunc("Fractal", [](const PushConstantBinder& b, const void*) {
        FractalUpdateSet::FractalPushData d{};   // max_steps по умолчанию — потолок рэймарча
        d.time = (float)SDL_GetTicks() / 1000.0f;
        b.PushFragment(d);
    });

    sm->CreatePushFunc("Mandelbrot", [](const PushConstantBinder& b, const void*) {
        FractalUpdateSet::FractalPushData d{};
        d.time = (float)SDL_GetTicks() / 1000.0f;
        // Потолок итераций пикселя = длина референс-орбиты: глубже неё дельты всё
        // равно не уходят (ре-базирование заворачивает m на начало орбиты).
        d.max_steps = FractalUpdateSet::MANDELBROT_ORBIT_MAX;
        b.PushFragment(d);
    });
}
