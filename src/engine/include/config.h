#pragma once
#include <SDL3/SDL.h>


//static constexpr Uint32 BASE_TB_SIZE = 1 * 1024 * 1024; // MB
// 3, не 2: при асинхронной загрузке слотов нужно одновременно A = на рендере (lr),
// B = загрузка в полёте (UPLOADING), C = слот, который готовит sim. При 2x async
// вырождается в синхронный — «следующего» слота просто нет, он занят под lr.
static constexpr int BUFFERING_LEVEL = 3;

// ── ДИАГНОСТИКА контеншена store (разбор 2026-07-05): отключают фазы конвейера,
//    чтобы замерить время store в профайлере без нагрузки рендера/аплоада на память
//    и PCIe. В UPS_priority sim продолжает готовить кадры (frame skip переиспользует
//    неотрисованные слоты), так что `_DefaultTransformBuffer .store` меряется непрерывно.
//    В норме ОБА false.
//      DISABLE_RENDER — не стартовать render+fence потоки (нет рендер-GPU-работы);
//      DISABLE_UPLOAD — не стартовать upload; PrepareFunc гоняет store БЕЗ submit на GPU
//                       (нет upload-DMA), слот прокручивается вручную для frame skip.
static constexpr bool DISABLE_RENDER = false;
static constexpr bool DISABLE_UPLOAD = false;
