// ============================================================================
//  Sandbox: полная череда двух очередей — заливка на КОПИРОВАЛЬНОЙ, отрисовка на
//  ГРАФИЧЕСКОЙ, между ними блокирующий fence.
//
//  Проверяет то, что до этого зонда не исполнялось ни разу: движок берёт все
//  командные буферы с ролью GRAPHICS, поэтому не-графические пути форка (три
//  семьи, сабмит в свою очередь, маски барьеров, CONCURRENT у буферов) жили
//  непроверенными.
//
//  ЧТО ЗДЕСЬ ДВИЖКОВОЕ. Всё, кроме конвейера исполнения: BufferData +
//  UpdateInstruction + ReadBackInstruction, TransferManager под transfer-буферы,
//  ShaderManager (vs/fs + ShaderProgram), PassManager (RenderPassStep со
//  свопчейном-атласом), PipeManager (графический пайплайн). Свой только порядок
//  вызовов: ни потоков, ни слотов — последовательно, через fence.
//
//  ПОЧЕМУ ТРЕУГОЛЬНИК РИСУЕТСЯ ЗАЛИТЫМИ ВЕРШИНАМИ. Это и есть проверка
//  межсемейного обмена: буфер ЗАПОЛНЕН на копировальной семье, а ЧИТАЕТСЯ на
//  графической. Ровно тот случай, ради которого буферы переведены в
//  VK_SHARING_MODE_CONCURRENT: при EXCLUSIVE содержимое на второй семье было бы
//  формально undefined — и это не гонка, fence её и так закрывает.
//
//  Вершинный буфер — стрим пула (_VertexPosBuffer, FMT_PosStream). Его usage
//  (VERTEX) объявляет CreateVertexShader, руками ничего не помечается. Заодно
//  это самый острый случай для масок барьеров: состояние по умолчанию такого
//  буфера переводится в VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, которой на
//  копировальной очереди НЕ СУЩЕСТВУЕТ — без маски барьер уронил бы устройство.
//
//  КРИТЕРИИ (все независимы):
//    1. в логе два разных 'first submit to queue family': копировальная и графическая;
//    2. ридбэк привозит записанные вершины (заливка реально доехала);
//    3. валидация Vulkan молчит — ПОЛНОСТЬЮ, включая выход;
//    4. на экране градиентный треугольник — цвет выводится ИЗ ПОЗИЦИИ, поэтому
//       занулённый буфер дал бы вырожденную геометрию, а не картинку.
//
//  ВЫХОД — ПО ЗАКРЫТИЮ ОКНА (или Esc), а не по счётчику кадров. Раньше зонд жил
//  фиксированные 90 кадров, то есть полторы секунды, и самопроизвольно
//  исчезающее окно вместе с руганью валидации на выходе выглядело неотличимо от
//  падения. Ни того, ни другого больше нет; жёсткая крышка по кадрам осталась
//  только предохранителем для запуска без присмотра.
//
//  Запуск (рабочая директория — src/sandbox или корень репо, путь к шейдерам
//  подстрахован кандидатами):
//    SDL_LOGGING=*=info                        — иначе строк про очереди не видно
//    VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
//    VK_KHRONOS_VALIDATION_VALIDATE_SYNC=1
// ============================================================================
#include <SDL3/SDL.h>
#include <cstdint>
#include <string>
#include <vector>

#include "config.h"
#include "TransferManager.h"
#include "BufferManager.h"
#include "BufferUpdateStruct.h"
#include "QueueManager.h"
#include "ShaderManager.h"
#include "ShaderData.h"
#include "ShaderTypes.h"
#include "RenderManager.h"
#include "RenderCommandData.h"
#include "PipeManager.h"
#include "PositionStructure.h"
#include "TextureData.h"

// Дискретная карта на переключаемой графике. Тот же экспорт, что в src/game/src/main.cpp:
// без него драйвер отдаёт зонду встройку, и мерить/проверять пришлось бы не на той карте,
// на которой работает игра.
extern "C" __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

namespace {

constexpr uint32_t VERTEX_COUNT = 3;
constexpr uint32_t VERTEX_BYTES = VERTEX_COUNT * sizeof(PosOnly);   // float3, страйд 12

// Треугольник сразу в clip-пространстве: ни камеры, ни трансформа (см. шапку шейдера).
constexpr PosOnly TRIANGLE[VERTEX_COUNT] = {
    {  0.0f, -0.6f, 0.0f },
    {  0.6f,  0.6f, 0.0f },
    { -0.6f,  0.6f, 0.0f },
};

bool     g_readback_ok = false;
uint32_t g_frames_drawn = 0;

// Шейдеры ищем от нескольких корней: рабочая директория у зондов исторически разная.
// Возврат ПО ЗНАЧЕНИЮ намеренно: со статическим буфером второй вызов затирал результат первого.
std::string FindShader(const char* leaf)
{
    const char* roots[] = {
        "../sandbox/shaders_code/", "shaders_code/",
        "src/sandbox/shaders_code/", "../../src/sandbox/shaders_code/",
    };
    for (const char* r : roots) {
        std::string p = std::string(r) + leaf;
        if (SDL_GetPathInfo(p.c_str(), nullptr)) return p;
    }
    return {};
}

}   // namespace

int main(int, char**)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) { SDL_Log("SDL_Init: %s", SDL_GetError()); return 1; }

    auto make_window = [] {
        return SDL_CreateWindow("queue probe: transfer upload -> graphics draw", 640, 480, 0);
    };
    SDL_Window* win = make_window();
    if (!win) { SDL_Log("SDL_CreateWindow: %s", SDL_GetError()); return 1; }

    // debug=true обязателен: под ним SDL включает валидацию и печатает, в какую семью ушёл
    // первый сабмит. Без этого зонд ничего не докажет.
    SDL_GPUDevice* dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, true, nullptr);
    if (!dev) { SDL_Log("SDL_CreateGPUDevice: %s", SDL_GetError()); return 1; }
    SDL_ClaimWindowForGPUDevice(dev, win);

    // БАГ SDL 3.4.14 «первого окна»: claim возвращает true, но окно не регистрируется, и
    // свопчейн потом отвечает «Must claim window before…». Проверяем ФАКТ (формат свопчейна),
    // как в src/game/src/main.cpp, и один раз пересоздаём окно.
    if (SDL_GetGPUSwapchainTextureFormat(dev, win) == SDL_GPU_TEXTUREFORMAT_INVALID) {
        SDL_Log("Claim не сработал (баг первого окна SDL 3.4) — пересоздаю окно");
        SDL_ReleaseWindowFromGPUDevice(dev, win);
        SDL_DestroyWindow(win);
        win = make_window();
        if (!win || !SDL_ClaimWindowForGPUDevice(dev, win)) {
            SDL_Log("Повторный claim не удался: %s", SDL_GetError());
            return 1;
        }
    }
    const SDL_GPUTextureFormat swap_fmt = SDL_GetGPUSwapchainTextureFormat(dev, win);
    SDL_Log("Драйвер: %s", SDL_GetGPUDeviceDriver(dev));

    const std::string vs_path = FindShader("triangle.vert.hlsl");
    const std::string fs_path = FindShader("triangle.frag.hlsl");
    if (vs_path.empty() || fs_path.empty()) {
        SDL_Log("Не найдены шейдеры треугольника (жду CWD = src/sandbox или корень репо).");
        return 1;
    }

    {   // менеджеры — во вложенной области: их деструкторы освобождают GPU-ресурсы и обязаны
        // отработать ДО SDL_DestroyGPUDevice, иначе валидация ругается на живые объекты.
        QueueManager    qm(dev);
        TransferManager trm(dev);
        BufferManager   bm(dev, &trm);
        ShaderManager   sm(dev);
        PassManager     pm;
        PipeManager     pipes(dev, win);

        // Вершинный буфер заводить НЕ НУЖНО: стримы пула (_VertexPosBuffer и соседей) создаёт
        // сам конструктор BufferManager — Static, ~2.2 МБ, RESIZE_AND_COPY. usage ему объявит
        // вершинник ниже, GPU-буфер создаст BakePending.

        // ── Проход: цель — свопчейн-атлас PassManager'а (движок отдаёт его как обычный атлас) ──
        SDL_GPUGraphicsPipeline** pipeline_slot = new SDL_GPUGraphicsPipeline*(nullptr);
        RenderPassTexturesInfo rptd;
        rptd.CreateColorTextureInfo(SDL_GPU_LOADOP_CLEAR, SDL_GPU_STOREOP_STORE,
                                    SDL_FColor{ 0.06f, 0.06f, 0.09f, 1.0f }, swap_fmt);
        rptd.SetColorTexture(pm.GetSwapchainAtlas(), 0);

        RenderPassStep* tri_pass = pm.CreateRenderPass(
            "TRIANGLE_PASS",
            [&bm, pipeline_slot](SDL_GPUCommandBuffer* cb, PassManager*, RenderPassStep& rp)
        {
            // Резолв таргетов — на исполнении: текстура свопчейна меняется каждый кадр.
            // В движке это делает RenderPassStandardBody; здесь тело своё, потому что
            // штатное рисует ИНСТАНСНЫМ ИНДИРЕКТОМ по слепку батчей, а у зонда ни моделей,
            // ни инстансов, ни каллинга нет — и заводить их ради треугольника незачем.
            rp.renderPassTexsData.ResolveTargets();
            if (!rp.renderPassTexsData.colorTargetInfos[0].texture) return;   // кадра нет

            SDL_GPURenderPass* sdl_rp = SDL_BeginGPURenderPass(
                cb, rp.renderPassTexsData.colorTargetInfos.data(),
                (Uint32)rp.renderPassTexsData.colorTargetInfos.size(), nullptr);

            BufferData* vb = bm.GetBufferData(GeometryStreams::VERTEX_POS_BUFFER);
            if (*pipeline_slot && vb && vb->Static.buffer) {
                SDL_BindGPUGraphicsPipeline(sdl_rp, *pipeline_slot);
                SDL_GPUBufferBinding vbind{};
                vbind.buffer = vb->Static.buffer;
                vbind.offset = 0;
                SDL_BindGPUVertexBuffers(sdl_rp, 0, &vbind, 1);
                SDL_DrawGPUPrimitives(sdl_rp, VERTEX_COUNT, 1, 0, 0);
                ++g_frames_drawn;
            }

            SDL_EndGPURenderPass(sdl_rp);
        },
            std::move(rptd), 0);

        // ── Шейдеры и программа. Вершинник перечисляет ПОТРЕБЛЯЕМЫЕ стримы: это и есть
        //    декларация usage=VERTEX для _VertexPosBuffer (см. BufferData.h) ──
        sm.CreateVertexShader("triangle_vs", vs_path.c_str(),
                              { GeometryStreams::VERTEX_POS_BUFFER }, &bm);
        sm.CreateFragmentShader("triangle_fs", fs_path.c_str());

        ShaderProgramDescription spd;
        spd.cull_mode   = SDL_GPU_CULLMODE_NONE;
        spd.depth_test  = false;   // depth-таргета у прохода нет
        spd.depth_write = false;
        ShaderProgram* sp = sm.CreateShaderProgram(
            "sp_triangle", spd, tri_pass,
            "triangle_vs", {},    // vs-буферов нет: позиция приходит вершинным стримом, не storage
            "triangle_fs", {},    // fs-буферов нет
            {},                   // слотов текстур нет
            &bm);
        if (!sp) { SDL_Log("CreateShaderProgram не вернул программу."); return 1; }

        // Бейк создаёт GPU-буфер по объявленному usage; затем пайплайн и порядок проходов.
        bm.BakePending();
        BufferData* vb = bm.GetBufferData(GeometryStreams::VERTEX_POS_BUFFER);
        if (!vb || !vb->Static.buffer) { SDL_Log("BakePending не создал вершинный буфер."); return 1; }

        pipes.CreateGraphicsPiplenes(sm.GetShaderPrograms(), &sm);
        *pipeline_slot = pipes.GetGraphicPipeline(sp);
        if (!*pipeline_slot) { SDL_Log("Пайплайн не собрался — рисовать нечем."); return 1; }
        pm.FillRenderPasses();

        // ── Инструкции жизненного цикла буфера (как DefaultUpdateSet) ──
        bm.CreateUpdateInstruction(GeometryStreams::VERTEX_POS_BUFFER,
            [](SDL_GPUCopyPass*, BufferManager* bmm, UploadTask& task)
        {
            void* dst = bmm->AcquireTransferWritePtr(&task, VERTEX_BYTES);
            if (!dst) { SDL_Log("Не получил transfer-память под вершины."); return; }
            SDL_memcpy(dst, TRIANGLE, VERTEX_BYTES);
        },
            []() -> uint32_t { return VERTEX_BYTES; });

        bm.CreateReadBackInstruction(GeometryStreams::VERTEX_POS_BUFFER,
            [](BufferManager* bmm, ReadBackTask& task)
        {
            auto span = bmm->ReadFromTransferBuffer(&task, VERTEX_BYTES);
            if (span.size() != VERTEX_BYTES) { SDL_Log("Readback: %u байт вместо %u.",
                                                      (uint32_t)span.size(), VERTEX_BYTES); return; }
            g_readback_ok = SDL_memcmp(span.data(), TRIANGLE, VERTEX_BYTES) == 0;
        },
            []() -> uint32_t { return VERTEX_BYTES; });

        // ══ ФАЗА 1 — ЗАЛИВКА НА КОПИРОВАЛЬНОЙ ОЧЕРЕДИ ══
        SDL_Log("--- фаза 1: заливка вершин (upload queue) ---");
        {
            UploadCommandBuffer cb = qm.GetUploadQueue().AcquireCommandBuffer();
            if (!cb) { SDL_Log("Acquire (upload): %s", SDL_GetError()); return 1; }

            UploadCopyPass cp = cb.BeginBufferCopyPass();
            TransferBufferData* tbd = bm.ExecuteUpdateInstructions(cp.Raw());
            bm.ExecuteUploadTasks(cp.Raw(), 0);   // слотов нет — индекс 0
            cp.End();

            SDL_GPUFence* fence = cb.SubmitAndAcquireFence();
            if (!fence) { SDL_Log("Submit (upload): %s", SDL_GetError()); return 1; }
            // БЛОКИРУЮЩЕЕ ожидание: рендер стартует строго после того, как заливка ЗАКОНЧИЛАСЬ.
            // Это же и половина «пусть увидит» в барьере — на копировальной очереди её назвать
            // нечем (стадии вершинной выборки там нет), и закрывает её именно fence.
            SDL_WaitForGPUFences(dev, true, &fence, 1);
            SDL_ReleaseGPUFence(dev, fence);
            trm.ReleaseTB(tbd);   // только ПОСЛЕ fence: до него GPU ещё читает transfer-буфер
        }

        // ══ ФАЗА 2 — ЧТЕНИЕ ОБРАТНО, ТОЖЕ НА КОПИРОВАЛЬНОЙ ══
        SDL_Log("--- фаза 2: чтение вершин обратно (upload queue) ---");
        {
            UploadCommandBuffer cb = qm.GetUploadQueue().AcquireCommandBuffer();
            UploadCopyPass cp = cb.BeginBufferCopyPass();
            TransferBufferData* tbd = bm.ExecuteReadBackInstructionsSize();
            bm.ExecuteDownloadTasks(cp.Raw(), 0);
            cp.End();

            SDL_GPUFence* fence = cb.SubmitAndAcquireFence();
            SDL_WaitForGPUFences(dev, true, &fence, 1);
            SDL_ReleaseGPUFence(dev, fence);
            bm.ExecuteReadBackInstructionsReader();   // читатель — ПОСЛЕ fence
            trm.ReleaseTB(tbd);
        }

        // ══ ФАЗА 3 — ОТРИСОВКА НА ГРАФИЧЕСКОЙ ОЧЕРЕДИ ══
        // Вершины уже на месте (fence фазы 1 отработал), поэтому графическая очередь читает
        // буфер, ЗАПОЛНЕННЫЙ ЧУЖОЙ СЕМЬЁЙ. Ровно ради этого буферы и переведены в CONCURRENT.
        // Крутимся, ПОКА ОКНО НЕ ЗАКРОЮТ, а не фиксированное число кадров: зонд визуальный, и
        // самопроизвольно исчезающее через полторы секунды окно неотличимо от падения.
        // Предохранитель на случай запуска без присмотра — жёсткая крышка по кадрам.
        SDL_Log("--- фаза 3: отрисовка (render queue). Закрой окно, чтобы выйти. ---");
        const int FRAME_CAP = 60 * 60 * 5;   // ~5 минут при vsync
        bool quit = false;
        for (int i = 0; i < FRAME_CAP && !quit; ++i) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_EVENT_QUIT ||
                    ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit = true;
                if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE) quit = true;
            }
            if (quit) break;

            RenderCommandBuffer cb = qm.GetRenderQueue().AcquireCommandBuffer();
            if (!cb) { SDL_Log("Acquire (render): %s", SDL_GetError()); break; }

            SDL_GPUTexture* tex = nullptr;
            Uint32 w = 0, h = 0;
            if (!cb.AcquireSwapchainTexture(win, &tex, &w, &h) || !tex) {
                cb.Cancel();   // кадра нет — буфер вернуть в пул, иначе утечёт
                continue;
            }

            pm.SetSwapchain(tex, w, h);
            pm.ExecutePassesSteps(cb.Raw(), 0);

            SDL_GPUFence* fence = cb.SubmitAndAcquireFence();
            if (!fence) { SDL_Log("Submit (render): %s", SDL_GetError()); break; }
            SDL_WaitForGPUFences(dev, true, &fence, 1);   // без слотов — ждём каждый кадр
            SDL_ReleaseGPUFence(dev, fence);
        }

        // Именованные шейдеры держат GPU-модули в реестрах ShaderManager, а те разрушаются ПОСЛЕ
        // гашения его токена — делитер тогда сознательно становится no-op (чтобы не звать релиз
        // по мёртвому устройству), и модули доживают до vkDestroyDevice. Сносим их явно, пока
        // менеджер жив: иначе валидация на выходе сыпет VUID-vkDestroyDevice-device-05137, и её
        // молчание перестаёт быть годным критерием.
        sm.DeleteShaderProgram("sp_triangle");
        sm.DeleteVertexShader("triangle_vs");
        sm.DeleteFragmentShader("triangle_fs");

        // ── Вердикт ──
        SDL_Log("Вершины после ридбэка: %s", g_readback_ok ? "СОВПАЛИ" : "НЕ СОВПАЛИ");
        SDL_Log("Кадров с отрисованным треугольником: %u", g_frames_drawn);
        if (g_readback_ok && g_frames_drawn > 0)
            SDL_Log("ВЕРДИКТ: заливка прошла копировальной очередью, отрисовка — графической, "
                    "и графика прочитала буфер, заполненный чужой семьёй.");
        else
            SDL_Log("ВЕРДИКТ: ПРОВАЛ. Смотри строки 'GPU queue families' / "
                    "'GPU first submit to queue family N' и вывод валидации.");
        SDL_Log("В логе должно быть ДВА разных 'first submit to queue family' — "
                "копировальная (фаза 1) и графическая (фаза 3).");

        delete pipeline_slot;
    }   // ← менеджеры разрушены здесь, устройство ещё живо

    SDL_ReleaseWindowFromGPUDevice(dev, win);
    SDL_DestroyGPUDevice(dev);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return (g_readback_ok && g_frames_drawn > 0) ? 0 : 1;
}
