// ============================================================================
//  Sandbox: доезжает ли заливка буфера до ВЫДЕЛЕННОЙ КОПИРОВАЛЬНОЙ очереди.
//
//  Первая настоящая проверка всей цепочки очередей. До неё каждый шаг форка
//  (три семьи, сабмит в свою очередь, маски барьеров, CONCURRENT у буферов)
//  проверялся только на отсутствие регрессии: движок берёт все командные буферы
//  с ролью GRAPHICS, поэтому не-графические пути ни разу не исполнялись.
//
//  Здесь заливка идёт через QueueManager::GetUploadQueue(), то есть с ролью
//  SDL_GPU_QUEUETYPE_TRANSFER, а всё остальное — движковыми системами, как в
//  Engine_Frame: BufferData + UpdateInstruction + ReadBackInstruction,
//  TransferManager под transfer-буферы. Своего тут только конвейер исполнения:
//  ни потоков, ни слотов, строго последовательные вызовы через fence.
//
//  ПОЧЕМУ БУФЕР ПОМЕЧЕН VERTEX. Это самый острый случай для масок барьеров.
//  Состояние по умолчанию такого буфера — VERTEX_READ, а SDL переводит его в
//  стадию VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, КОТОРОЙ НА КОПИРОВАЛЬНОЙ ОЧЕРЕДИ
//  НЕ СУЩЕСТВУЕТ. Без маски барьер «возврат в умолчание» после копирования
//  назвал бы её и уронил устройство. То есть зонд проверяет не только
//  маршрутизацию, но и ровно тот сценарий, ради которого маска писалась.
//
//  КРИТЕРИЙ — три независимых:
//    1. в логе SDL «first submit to queue family N» с N != графической;
//    2. валидация Vulkan молчит (запускать с включённым слоем);
//    3. ридбэк привозит записанный узор — данные реально доехали, а не «не
//       упало». Проверка по СОДЕРЖИМОМУ, а не по отсутствию ошибок.
//
//  Запуск: рабочая директория любая (ассеты не нужны, шейдеров нет).
//    SDL_LOGGING=*=info                        — иначе строки про очереди не видно
//    VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
//    VK_KHRONOS_VALIDATION_VALIDATE_SYNC=1
// ============================================================================
#include <SDL3/SDL.h>
#include <cstdint>
#include <vector>

#include "config.h"
#include "TransferManager.h"
#include "BufferManager.h"
#include "BufferUpdateStruct.h"
#include "QueueManager.h"

namespace {

constexpr const char* PROBE_BUFFER = "__probe_transfer_buffer";
constexpr uint32_t     NUM_ELEMENTS = 4096;
constexpr uint32_t     BUF_BYTES    = NUM_ELEMENTS * sizeof(uint32_t);

// Узор, который должен доехать. Не константа: константу привезёт и занулённая
// VRAM, а индекс-зависимое значение — только настоящая заливка.
constexpr uint32_t Pattern(uint32_t i) { return i * 2654435761u + 0x9E3779B9u; }

bool g_readback_ok = false;
uint32_t g_first_bad = 0;

}   // namespace

int main(int, char**)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) { SDL_Log("SDL_Init: %s", SDL_GetError()); return 1; }

    // Окно нужно не для картинки, а чтобы устройство выбиралось так же, как в игре:
    // выбор семьи очередей завязан на поддержку презента (SDL_Vulkan_GetPresentationSupport).
    SDL_Window* win = SDL_CreateWindow("transfer queue probe", 320, 240, SDL_WINDOW_HIDDEN);
    if (!win) { SDL_Log("SDL_CreateWindow: %s", SDL_GetError()); return 1; }

    // debug=true обязателен: под ним SDL включает слой валидации и печатает,
    // в какую семью ушёл первый сабмит. Без него зонд не сможет ничего доказать.
    SDL_GPUDevice* dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, true, nullptr);
    if (!dev) { SDL_Log("SDL_CreateGPUDevice: %s", SDL_GetError()); return 1; }
    if (!SDL_ClaimWindowForGPUDevice(dev, win)) { SDL_Log("ClaimWindow: %s", SDL_GetError()); return 1; }
    SDL_Log("Драйвер: %s", SDL_GetGPUDeviceDriver(dev));

    // Менеджеры — во ВЛОЖЕННОЙ области: их деструкторы освобождают GPU-буферы, и сделать это
    // они обязаны ДО SDL_DestroyGPUDevice. Иначе валидация справедливо ругается на живые
    // VkBuffer при разрушении устройства, и её вывод перестаёт быть критерием чистоты зонда.
    {
    QueueManager    qm(dev);
    TransferManager trm(dev);
    BufferManager   bm(dev, &trm);

    // ── Буфер движковым путём ──
    bm.CreateBufferData(PROBE_BUFFER, BUF_BYTES, BufferDataType::Static, ResizeBehaviour::RESIZE_ONLY);
    // Ручной тег usage — как INDIRECT у владельца индирект-буфера: деклараций (шейдеров)
    // тут нет, а без usage BakePending буфер пропускает. VERTEX выбран намеренно, см. шапку.
    bm.GetBufferData(PROBE_BUFFER)->usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    bm.BakePending();
    if (!bm.GetBufferData(PROBE_BUFFER)->Static.buffer) {
        SDL_Log("BakePending не создал GPU-буфер — дальше смысла нет.");
        return 1;
    }

    // ── Инструкции жизненного цикла (как DefaultUpdateSet) ──
    bm.CreateUpdateInstruction(PROBE_BUFFER,
        [](SDL_GPUCopyPass*, BufferManager* bmm, UploadTask& task)
    {
        uint32_t* dst = static_cast<uint32_t*>(bmm->AcquireTransferWritePtr(&task, BUF_BYTES));
        if (!dst) { SDL_Log("Не получил transfer-память под узор."); return; }
        for (uint32_t i = 0; i < NUM_ELEMENTS; ++i) dst[i] = Pattern(i);
    },
        []() -> uint32_t { return BUF_BYTES; });

    bm.CreateReadBackInstruction(PROBE_BUFFER,
        [](BufferManager* bmm, ReadBackTask& task)
    {
        auto span = bmm->ReadFromTransferBuffer(&task, BUF_BYTES);
        if (span.size() != BUF_BYTES) {
            SDL_Log("Readback: %u байт вместо %u.", (uint32_t)span.size(), BUF_BYTES);
            return;
        }
        const uint32_t* v = reinterpret_cast<const uint32_t*>(span.data());
        for (uint32_t i = 0; i < NUM_ELEMENTS; ++i) {
            if (v[i] != Pattern(i)) { g_first_bad = i; return; }
        }
        g_readback_ok = true;
    },
        []() -> uint32_t { return BUF_BYTES; });

    // ══ ФАЗА 1: ЗАЛИВКА НА КОПИРОВАЛЬНОЙ ОЧЕРЕДИ ══
    // Роль передаёт обёртка: UploadQueue просит SDL_GPU_QUEUETYPE_TRANSFER, и командный
    // буфер приходит из пула ТОЙ семьи. Ни одной ветки «а есть ли такая очередь» —
    // если выделенной семьи нет, SDL сам отдаст ближайшую более широкую.
    SDL_Log("--- фаза 1: заливка (upload queue) ---");
    {
        UploadCommandBuffer cb = qm.GetUploadQueue().AcquireCommandBuffer();
        if (!cb) { SDL_Log("AcquireCommandBuffer (upload): %s", SDL_GetError()); return 1; }

        UploadCopyPass cp = cb.BeginBufferCopyPass();
        TransferBufferData* tbd = bm.ExecuteUpdateInstructions(cp.Raw());
        bm.ExecuteUploadTasks(cp.Raw(), 0);   // слотов нет — индекс 0
        cp.End();

        SDL_GPUFence* fence = cb.SubmitAndAcquireFence();
        if (!fence) { SDL_Log("SubmitAndAcquireFence (upload): %s", SDL_GetError()); return 1; }
        SDL_WaitForGPUFences(dev, true, &fence, 1);
        SDL_ReleaseGPUFence(dev, fence);
        trm.ReleaseTB(tbd);   // только ПОСЛЕ fence: до него GPU ещё читает transfer-буфер
    }

    // ══ ФАЗА 2: ЧТЕНИЕ ОБРАТНО, ТОЖЕ НА КОПИРОВАЛЬНОЙ ══
    // Скачивание — такое же копирование, копировальная очередь его умеет. Заодно это
    // проверяет обратное направление: буфер как ИСТОЧНИК копии, а не только приёмник.
    SDL_Log("--- фаза 2: чтение обратно (upload queue) ---");
    {
        UploadCommandBuffer cb = qm.GetUploadQueue().AcquireCommandBuffer();
        if (!cb) { SDL_Log("AcquireCommandBuffer (readback): %s", SDL_GetError()); return 1; }

        UploadCopyPass cp = cb.BeginBufferCopyPass();
        TransferBufferData* tbd = bm.ExecuteReadBackInstructionsSize();
        bm.ExecuteDownloadTasks(cp.Raw(), 0);
        cp.End();

        SDL_GPUFence* fence = cb.SubmitAndAcquireFence();
        if (!fence) { SDL_Log("SubmitAndAcquireFence (readback): %s", SDL_GetError()); return 1; }
        SDL_WaitForGPUFences(dev, true, &fence, 1);
        SDL_ReleaseGPUFence(dev, fence);

        bm.ExecuteReadBackInstructionsReader();   // читатель — ПОСЛЕ fence
        trm.ReleaseTB(tbd);
    }

    // ── Вердикт ──
    if (g_readback_ok) {
        SDL_Log("ВЕРДИКТ: узор доехал целиком (%u элементов). Заливка и чтение прошли "
                "по запрошенной роли, барьеры не уронили устройство.", NUM_ELEMENTS);
    } else {
        SDL_Log("ВЕРДИКТ: ПРОВАЛ — первое расхождение на индексе %u. "
                "Данные не доехали: смотри строки про семьи очередей и вывод валидации.",
                g_first_bad);
    }
    SDL_Log("Сверься с логом: 'GPU queue families' (какая семья копировальная) и "
            "'GPU first submit to queue family N' (куда реально ушла работа). "
            "N должен быть НЕ графическим — иначе роль не доехала.");
    }   // ← менеджеры разрушены здесь, устройство ещё живо

    SDL_ReleaseWindowFromGPUDevice(dev, win);
    SDL_DestroyGPUDevice(dev);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return g_readback_ok ? 0 : 1;
}
