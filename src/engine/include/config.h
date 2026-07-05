#pragma once
#include <SDL3/SDL.h>


//static constexpr Uint32 BASE_TB_SIZE = 1 * 1024 * 1024; // MB
// 3, не 2: при асинхронной загрузке слотов нужно одновременно A = на рендере (lr),
// B = загрузка в полёте (UPLOADING), C = слот, который готовит sim. При 2x async
// вырождается в синхронный — «следующего» слота просто нет, он занят под lr.
static constexpr int BUFFERING_LEVEL = 3;
