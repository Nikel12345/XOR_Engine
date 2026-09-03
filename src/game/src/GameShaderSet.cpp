#include "PCH.h"
#include "GameShaderSet.h"

void GameShaderSet::RegisterShaderFuncs(EngineContext*)
{
    // Пусто: у sp города своих push-констант нет, а движковые (счётчик светов) приезжают ТИПОВЫМИ
    // пушами — их объявляет маркером сам движковый пролог, который эти шейдеры включают. Появится
    // своя константа — регистрировать её здесь: sm->CreatePushInstruction("<имя sp>", стадия, …)
    // для одной программы либо sm->RegisterPushKind("<тип>", …) + маркер //@push в своём шейдере,
    // если её должны получать все, кто включает твой хедер (образец — mygame/FractalShaderSet.cpp).
}
