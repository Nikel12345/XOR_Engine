#pragma once

class EngineContext;
class LightDataModule;

// GPU-отсев и выбор уровня LOD для штатных проходов. Без вызова кадр рисуется по PIB и
// in-состоянию indirect, с ним — те же команды переписываются на месте по выжившим записям.
namespace DefaultCullingSet {
    // Из MainInit игры, ДО первой загрузки сцены: подстановка PIB → out_pib в проходах действует
    // при сборке батчей, а порядок создания программ задаёт порядок их исполнения.
    void Enable(EngineContext* ctx, LightDataModule* ldm);
}
