#include "PCH.h"
#include "BufferManager.h"
#include "CameraStruct.h"
#include "LightStruct.h"
#include "PositionStructure.h"

BufferManager::BufferManager(SDL_GPUDevice* device, TransferManager* transfer_manager) : dev(device), trm(transfer_manager) {
    using namespace DefaultBuffersNames;
	// Вершинных/индексного буферов тут НЕТ: их заводит ModelManager::CreateGeometryPool вместе с
	// именами стримов и инструкциями заливки — буфер геометрии не существует отдельно от пула.
	CreateBufferData(DEFAULT_TRANSFORM_BUFFER, BASE_TB_SIZE / 10, BufferDataType::Dynamic);
	CreateBufferData(DEFAULT_LIGHT_BUFFER, sizeof(LightLayout) * 2, BufferDataType::Dynamic);
	CreateBufferData(DEFAULT_CAMERA_BUFFER, sizeof(CameraData), BufferDataType::Dynamic);
	CreateBufferData(DEFAULT_POSITION_INDEX_BUFFER, BASE_TB_SIZE / 16/ 10, BufferDataType::Dynamic);
	CreateBufferData(DEFAULT_INSTANCE_BUFFER, BASE_TB_SIZE / 80, BufferDataType::Dynamic);
	CreateBufferData(DEFAULT_LIGHT_CAMERA_BUFFER, sizeof(CameraData) * 6, BufferDataType::Dynamic);

	// Варианты текстур: префикс на строку + плоские ячейки состояний (см. TextureStateDataModule).
	// Dynamic, как все покадровые: Static — одно тело на все слоты, и заливка слота N писала бы
	// туда, откуда рендер читает слот N-1. usage не трогаем — он приходит декларацией sp,
	// которая эти буферы называет (Engine::InitDefaultShaders).
	// Стартовые размеры условны — все три растит EnsureBufferCapacity. Разреженный канал: rank
	// полноразмерный, но 8 байт на 32 строки; index и state живут по числу переключающихся.
	CreateBufferData(DEFAULT_TEX_STATE_RANK_BUFFER, sizeof(uint32_t) * 2 * 256, BufferDataType::Dynamic);
	CreateBufferData(DEFAULT_TEX_STATE_INDEX_BUFFER, sizeof(uint32_t) * 256, BufferDataType::Dynamic);
	CreateBufferData(DEFAULT_TEX_STATE_BUFFER, sizeof(uint32_t) * 256, BufferDataType::Dynamic);

    CreateBufferData(DEFAULT_INDIRECT_BUFFER, sizeof(SDL_GPUIndexedIndirectDrawCommand) * 10, BufferDataType::Dynamic)
        ->usage |= SDL_GPU_BUFFERUSAGE_INDIRECT;

    CreateBufferData(DEFAULT_BOUND_SPHERE_BUFFER, BASE_TB_SIZE / 40, BufferDataType::Dynamic);
    CreateBufferData(DEFAULT_OUT_PIB_BUFFER, BASE_TB_SIZE / 16 / 10, BufferDataType::Dynamic);
    CreateBufferData(DEFAULT_ENTITY_TO_CMD_BUFFER, BASE_TB_SIZE / 16 / 10, BufferDataType::Dynamic);

    // ── UI-текст: 4 буфера разреженного текст-канала (см. UI_DataModule).
    // Только РЕГИСТРАЦИЯ обёрток (имена+размеры). usage НЕ трогаем: флаги приходят из декларации
    // потребителя — программы "UI" (Engine::InitDefaultShaders), она называет эти буферы
    // фрагментными. Незабейканный буфер ЖДЁТ в pending_bakes сколько угодно кадров: BakePending
    // пропускает usage==0 и НЕ выкидывает из очереди, так что поздняя декларация его оживит.
    // ОБРАТНОЕ НЕВЕРНО: уже созданный буфер из очереди удаляется, и usage, добавленный после
    // этого, не применится молча — см. WARNINGS.md.
    CreateBufferData(UI_TEXT_RANK_BUFFER,     sizeof(uint32_t) * 2 * 64,  BufferDataType::Dynamic);
    CreateBufferData(UI_TEXT_INDEX_BUFFER,    sizeof(uint32_t) * 2 * 256, BufferDataType::Dynamic);
    CreateBufferData(UI_TEXT_BUFFER,          sizeof(uint32_t) * 4096,    BufferDataType::Dynamic);

    // GlyphUVL шрифта (FontManager::StoreGlyphUVL). Тоже обёртка без usage — флаг приходит от
    // декларации программы "UI" (как остальные UI-буферы).
    CreateBufferData(UI_FONT_UVL_BUFFER, sizeof(uint32_t) * 4 * 256, BufferDataType::Dynamic);
}

BufferData* BufferManager::CreateBufferData(BufferDataName name, Uint32 size, BufferDataType type, ResizeBehaviour resize_behaviour)
{
    auto it = buffers_data.find(name);
    if (it != buffers_data.end()) {
        SDL_Log("Buffer '%s' already exists, returning existing buffer data.", name);
        return it->second.get();
    }

    auto data = std::make_unique<BufferData>();
    data->type = type;
    data->resize_behaviour = resize_behaviour;
	data->debug_name = name;
    // usage здесь НЕ трогаем (остаётся 0): его наполнят декларации до бейка — см. BufferData.h.

    // ОТЛОЖЕННОЕ СОЗДАНИЕ: здесь только РАЗМЕРЫ, сам SDL_GPUBuffer создаст BakePending (начало
    // PrepareFunc). Потребителям это безразлично: они держат BufferData* и берут GPU-хэндл через
    // _GetGPUBufferForFrame на бинде — до бейка его просто никто не спрашивает.
    switch (type) {
        case BufferDataType::Static:
			data->Static.buffer_size = size;
            break;

        case BufferDataType::Dynamic:
            for (int i = 0; i < BUFFERING_LEVEL; i++) {
                data->Dynamic.buffer_size[i] = size;
            };
            break;
        }


    BufferData* ptr = data.get();
    buffers_data[name] = std::move(data);
    pending_bakes.push_back(ptr);

    return ptr;
}

// Дренаж отложенных созданий. Каждый кадр (начало PrepareFunc) — поэтому ресурс, заведённый
// игрой в любом кадре, живёт на GPU уже в этом же кадре. См. BufferManager.h.
void BufferManager::BakePending()
{
    if (pending_bakes.empty()) return;

    for (BufferData* data : pending_bakes) {
        if (!data) continue;
        // Ни одна декларация буфер не назвала — usage=0, SDL такой не примет. НЕ создаём, но и НЕ
        // выкидываем из очереди: если позже sp/материал/проход объявит usage, ближайший бейк создаст.
        if (data->usage == 0) continue;
        switch (data->type) {
        case BufferDataType::Static:
            if (!data->Static.buffer) {
                data->Static.buffer = CreateBuffer(data->Static.buffer_size, data->usage);
                if (!data->Static.buffer)
                    SDL_Log("BufferManager::BakePending: buffer '%s' failed.", data->debug_name.c_str());
            }
            break;

        case BufferDataType::Dynamic:
            for (int i = 0; i < BUFFERING_LEVEL; i++) {
                if (data->Dynamic.buffers[i]) continue;
                data->Dynamic.buffers[i] = CreateBuffer(data->Dynamic.buffer_size[i], data->usage);
                if (!data->Dynamic.buffers[i])
                    SDL_Log("BufferManager::BakePending: buffer '%s' [%d] failed.", data->debug_name.c_str(), i);
            }
            break;
        }
    }

    // Убираем из очереди ТОЛЬКО реально созданные. nullptr-ресурс (usage ещё не объявлен либо
    // создание не удалось) остаётся ждать следующего кадра — создание гардится по !buffer, дубля нет.
    std::erase_if(pending_bakes, [](BufferData* d) {
        if (!d) return true;
        if (d->type == BufferDataType::Static) return d->Static.buffer != nullptr;
        for (int i = 0; i < BUFFERING_LEVEL; i++)
            if (!d->Dynamic.buffers[i]) return false;
        return true;
    });
}

void BufferManager::TrashBuffers(uint64_t fences_done)
{
    // Дренаж на sim. Стамп при первом визите (позже момента постановки — консервативно, безопасно);
    // release после BUFFERING_LEVEL завершённых render-fence: fence одного queue сигналят в порядке
    // сабмита, значит все кадры, отправленные до стампа, к этому моменту дошли.
    auto it = trash.begin();
    while (it != trash.end()) {
        if (it->ready_at == 0) { it->ready_at = fences_done + BUFFERING_LEVEL; ++it; }
        else if (fences_done >= it->ready_at) {
            SDL_ReleaseGPUBuffer(dev, it->buf);
            it = trash.erase(it);
        }
        else ++it;
    }
}

BufferManager::~BufferManager()
{
    for (auto& [name, data] : buffers_data)
    {
        if (!data) continue;

        switch (data->type)
        {
        case BufferDataType::Static:
            if (data->Static.buffer)
                SDL_ReleaseGPUBuffer(dev, data->Static.buffer);
            break;

        case BufferDataType::Dynamic:
            for (int i = 0; i < BUFFERING_LEVEL; i++)
                if (data->Dynamic.buffers[i])
                    SDL_ReleaseGPUBuffer(dev, data->Dynamic.buffers[i]);
            break;
        }
    }

    buffers_data.clear();
}

SDL_GPUBuffer* BufferManager::CreateBuffer(Uint32 size, SDL_GPUBufferUsageFlags usage)
{
    SDL_GPUBufferCreateInfo info{};
    info.size = size;
    info.usage = usage;
    SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(dev, &info);
    if (!buffer) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Buffer creation failed: %s", SDL_GetError());
        return nullptr;
    }
    return buffer;
}

