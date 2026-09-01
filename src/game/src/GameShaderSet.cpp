#include "PCH.h"
#include "GameShaderSet.h"

void GameShaderSet::RegisterShaderFuncs(EngineContext*)
{
    // Пусто: игра пользуется только движковыми sp, их push-константы регистрирует сам движок
    // вместе с созданием программ (Engine::InitDefaultShaders). Появится своя sp из shaders.json —
    // её sm->CreatePushFunc("<имя sp>", …) писать сюда (образец — mygame/FractalShaderSet.cpp).
}
