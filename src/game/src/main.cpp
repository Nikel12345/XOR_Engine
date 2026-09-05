#include "PCH.h"
#include "Engine.h"
#include "Game.h"

extern "C" __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

int main() {
    EngineConfig cfg;
    cfg.title = "GPU-triangle (basic)";
    cfg.present_mode = SDL_GPU_PRESENTMODE_MAILBOX;

    // Настройки графики. render_scale — частота сэмплирования сцены относительно окна: 2.0 = SSAA 2x
    // (вчетверо больше пикселей, present-блит усредняет их вниз). Эффекты за ним не растут — SSAO и
    // блум остаются в оконном разрешении, им лишние сэмплы не нужны.
    cfg.graphics.render_scale = 1.0f;
    cfg.graphics.global_scale = 1.0f;   // общий сброс качества по ВСЕМ таргетам
    cfg.graphics.ssao_scale   = 0.5f;
    cfg.graphics.bloom_scale  = 0.5f;

    Engine engine(cfg);
    if (!engine.IsValid()) return 1;

    Game game(&engine);
    game.MainInit();

    engine.SetGameIterate([&game] { game.MainIterate(); });
    return engine.Run();
}
