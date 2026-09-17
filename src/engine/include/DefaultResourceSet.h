#pragma once

class EngineContext;
class BufferManager;
class TextureManager;

namespace DefaultResourceSet
{
    // Движковые дефолтные текстуры (albedo/normal/orm/emissive) в FallbackAtlas — чтобы редактор
    // мог создавать материалы и заполнять слоты по ролям без ассетов игры. Плюс примитивы
    // quad/sphere/cube: генерируются кодом, поэтому любая сцена ссылается на них по имени.
    // Зовётся из конструктора движка, ПЕРЕД набором шейдеров.
    void SetDefaultResources(EngineContext* ctx);

    // Дефолтные GPU-ресурсы движка: буферы его дата-модулей, сэмплеры, атлас текста. Зовутся
    // СРАЗУ после создания своего менеджера: инструкцию заливки регистрируют по имени буфера и
    // резолвят его в BufferData* на месте, поэтому буфер обязан появиться раньше инструкции.
    void CreateDefaultBuffers(BufferManager* bm);
    void CreateDefaultTextureResources(TextureManager* tm);
}
