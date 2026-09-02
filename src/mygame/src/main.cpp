#include "PCH.h"
#include "Engine.h"
#include "MyGame.h"

// Драйверу AMD этот символ нужен ИМЕННО в exe: из статической библиотеки движка линкер его не
// вытянет (на него никто не ссылается). Единственное, что не может уехать в Engine.
extern "C" __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 0;

int main() {
    EngineConfig cfg;
    cfg.title = "MyGame";
    cfg.present_mode = SDL_GPU_PRESENTMODE_IMMEDIATE;

    Engine engine(cfg);
    if (!engine.IsValid()) return 1;

    MyGame game(&engine);
    game.MainInit();   // ресурсы и сцена — до старта потоков внутри Run()

    // Крутится на sim-потоке. Игра живёт в этом кадре стека и разрушится после Run() —
    // Run() присоединяет потоки до возврата, поэтому висячего колбэка не остаётся.
    engine.SetGameIterate([&game] { game.MainIterate(); });
    return engine.Run();
}
