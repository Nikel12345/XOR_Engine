#pragma once

class EngineContext;

namespace DefaultResourceSet
{
    // Движковые дефолтные текстуры (albedo/normal/orm/emissive) в _FallbackAtlas — чтобы редактор
    // мог создавать материалы и заполнять слоты по ролям без ассетов игры. Плюс примитивы
    // quad/sphere/cube: генерируются кодом, поэтому любая сцена ссылается на них по имени.
    // Зовётся из конструктора движка, ПЕРЕД набором шейдеров.
    void SetDefaultResources(EngineContext* ctx);
}
