#include "PCH.h"
#include "GameShaderSet.h"
#include "EngineContext.h"
#include "ShaderManager.h"
#include "DefaultRenderPassSet.h"   // RP::LightCountPushData

void GameShaderSet::RegisterShaderFuncs(EngineContext* ctx)
{
    namespace RP = DefaultRenderPassNamespace;
    ShaderManager* sm = ctx->GetShaderManager();

    // Свои sp города (shaders.json сцены) стоят на ДВИЖКОВОЙ лайтинг-базе (их fs включает
    // main_pass.frag.hlsl / transparent.frag.hlsl), а она читает число источников света
    // push-константой в b0. Значит каждая такая программа обязана этот пуш зарегистрировать —
    // движок делает это только за свои (Engine::InitDefaultShaders). Без регистрации b0 займёт
    // таблица UVL, и материальные униформы уедут на регистр ниже.
    for (const char* lit_sp : { "LitTiled", "LitDecal", "LitTransparentTiled" })
        sm->CreatePushFunc<RP::LightCountPushData>(lit_sp,
            [](const PushConstantBinder& b, RP::LightCountPushData data) { b.PushFragment(data); });
}
