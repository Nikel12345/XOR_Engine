#include "PCH.h"
#include "Engine.h"
#include "MyGame.h"

extern "C" __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

int main() {
    EngineConfig cfg;
    cfg.title = "MyGame";
    cfg.present_mode = SDL_GPU_PRESENTMODE_MAILBOX;

    cfg.graphics.render_scale = 1.0f;   // SSAA
    cfg.graphics.global_scale = 1.0f;   // общий сброс качества по ВСЕМ таргетам
    cfg.graphics.ssao_scale   = 0.5f;
    cfg.graphics.bloom_scale  = 0.5f;

    Engine engine(cfg);
    if (!engine.IsValid()) return 1;

    MyGame game(&engine);
    game.MainInit();

    engine.SetGameIterate([&game] { game.MainIterate(); });
    return engine.Run();
}
