#pragma once

class EngineContext;

// Место под СВОИ код-байндинги шейдеров этой игры. Движковый набор compute-программ
// (каллинг/bloom/blur) живёт в движке — DefaultShaderSet.h.
namespace GameShaderSet
{
    // Push/dispatch — ИНСТРУКЦИИ ПО ИМЕНИ в реестре ShaderManager: зовётся ОДИН РАЗ на
    // инициализации, ДО первой LoadScene, а вешает функции на sp сам движок (и на создании
    // программы, и общим проходом в конце каждой загрузки). Своих sp у игры пока нет —
    // все её программы движковые (Engine::InitDefaultShaders), их пуши регистрирует движок.
    void RegisterShaderFuncs(EngineContext* ctx);
}
