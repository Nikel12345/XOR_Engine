#include "PCH.h"
#include "BufferManager.h"

void BufferManager::BindGPUIndexBuffer(SDL_GPURenderPass* rp, Uint32 offset)
{
    BufferData* data = GetBufferData(DefaultBuffersNames::DEFAULT_INDEX_BUFFER);
    if (!data) {
        SDL_Log("BindGPUIndexBuffer::Index buffer 'DefaultIndexBuffer' not found");
        return;
    }

    if (data->type != BufferDataType::Static) {
        SDL_Log("Index buffer 'DefaultIndexBuffer' is not static");
        return;
    }

    SDL_GPUBuffer* buf = data->Static.buffer;
    if (!buf) {
        SDL_Log("Index buffer 'DefaultIndexBuffer' has null buffer pointer");
        return;
    }

    SDL_GPUBufferBinding ibind{};
    ibind.buffer = buf;
    ibind.offset = offset;

    SDL_BindGPUIndexBuffer(rp, &ibind, SDL_GPU_INDEXELEMENTSIZE_32BIT);
}

// Вершинные стримы шейдер-батча: список объявлен вершинником (CreateVertexShader, перечисление
// имён буферов) и приезжает сюда из слепка. ПОРЯДОК СПИСКА = ПОРЯДОК СЛОТОВ пайплайна: биндим
// одним вызовом с нулевого слота. Любой сбой резолва — ПРОПУСК ВСЕГО бинда, не сдвиг: слот со
// сдвинутым буфером = чтение чужого страйда = UB (вызывающий обязан пропустить и draw).
bool BufferManager::BindGPUVertexBuffers(SDL_GPURenderPass* rp, const std::vector<BufferData*>& buffers_data_vec)
{
    if (buffers_data_vec.empty()) {
        SDL_Log("BindGPUVertexBuffers: empty vertex stream list");
        return false;
    }

    std::vector<SDL_GPUBufferBinding> binds;
    binds.reserve(buffers_data_vec.size());
    for (const BufferData* data : buffers_data_vec)
    {
        if (!data || data->type != BufferDataType::Static || !data->Static.buffer) {
            SDL_Log("BindGPUVertexBuffers: vertex stream '%s' is missing/not static/null — bind skipped",
                data ? data->debug_name.c_str() : "<null>");
            return false;
        }
        SDL_GPUBufferBinding vbind{};
        vbind.buffer = data->Static.buffer;
        vbind.offset = 0;
        binds.push_back(vbind);
    }

    SDL_BindGPUVertexBuffers(rp, 0, binds.data(), safe_u32(binds.size()));
    return true;
}


void BufferManager::BindGPUVertexStorageBuffers(SDL_GPURenderPass* rp, Uint32 slot, const std::vector<BufferData*>& buffers_data_vec, uint8_t frame)
{
    std::vector<SDL_GPUBuffer*> buffers;
    buffers.reserve(buffers_data_vec.size());

    for (const BufferData* data : buffers_data_vec)
    {
        SDL_GPUBuffer* buf = _GetGPUBufferForFrame(data, frame);
        if (buf)
            buffers.push_back(buf);
        else
            SDL_Log("Null or invalid buffer_data in BindGPUVertexStorageBuffers");
    }

    SDL_BindGPUVertexStorageBuffers(rp, slot, buffers.data(), safe_u32(buffers.size()));
}


void BufferManager::BindGPUVertexStorageBuffers(SDL_GPURenderPass* rp, Uint32 slot, std::initializer_list<const char*> names, uint8_t frame)
{
    std::vector<SDL_GPUBuffer*> buffers;
    buffers.reserve(names.size());

    for (const char* name : names)
    {
        auto it = buffers_data.find(name);
        if (it != buffers_data.end())
        {
            SDL_GPUBuffer* buf = _GetGPUBufferForFrame(it->second.get(), frame);
            if (buf)
                buffers.push_back(buf);
            else
                SDL_Log("Buffer '%s' is invalid (BindGPUVertexStorageBuffers)", name);
        }
        else {
            SDL_Log("BindGPUVertexStorageBuffers::Buffer '%s' not found", name);
        }
    }

    SDL_BindGPUVertexStorageBuffers(rp, slot, buffers.data(), safe_u32(buffers.size()));
}


void BufferManager::BindGPUFragmentStorageBuffers(
    SDL_GPURenderPass* rp,
    Uint32 slot,
    const std::vector<BufferData*>& buffers_data_vec,
    uint8_t render_frame)
{
    std::vector<SDL_GPUBuffer*> buffers;
    buffers.reserve(buffers_data_vec.size());

    for (const BufferData* data : buffers_data_vec)
    {
        SDL_GPUBuffer* buf = _GetGPUBufferForFrame(data, render_frame);
        if (buf)
            buffers.push_back(buf);
        else
            SDL_Log("Null or invalid buffer_data in BindGPUFragmentStorageBuffers");
    }

    SDL_BindGPUFragmentStorageBuffers(rp, slot, buffers.data(), safe_u32(buffers.size()));
}


void BufferManager::BindGPUFragmentStorageBuffers(SDL_GPURenderPass* rp, Uint32 slot, std::initializer_list<const char*> names, uint8_t frame)
{
    std::vector<SDL_GPUBuffer*> buffers;
    buffers.reserve(names.size());

    for (const char* name : names)
    {
        auto it = buffers_data.find(name);
        if (it != buffers_data.end())
        {
            SDL_GPUBuffer* buf = _GetGPUBufferForFrame(it->second.get(), frame);
            if (buf)
                buffers.push_back(buf);
            else
                SDL_Log("Buffer '%s' is invalid (BindGPUFragmentStorageBuffers)", name);
        }
        else {
            SDL_Log("BindGPUFragmentStorageBuffers::Buffer '%s' not found", name);
        }
    }

    SDL_BindGPUFragmentStorageBuffers(rp, slot, buffers.data(), safe_u32(buffers.size()));
}

std::vector<SDL_GPUStorageBufferReadWriteBinding> BufferManager::BuildBindGPUComputeRWBuffers(const std::vector<BufferData*>& buffers_data, uint8_t render_frame)
{
	std::vector<SDL_GPUStorageBufferReadWriteBinding> buffers;
	buffers.reserve(buffers_data.size());

    for (const BufferData* data : buffers_data)
    {
        SDL_GPUBuffer* buf = _GetGPUBufferForFrame(data, render_frame);
        if (buf) {
            //SDL_Log("Compute RW buffer ptr: %p", (void*)buf);
            buffers.emplace_back(buf, false);
        }
        else {
            SDL_Log("Null or invalid buffer_data in BindGPUComputeStorageBuffers");
        }
	}
    return buffers;
}

std::vector<SDL_GPUStorageBufferReadWriteBinding> BufferManager::BuildBindGPUComputeRWBuffers(std::initializer_list<const char*> names, uint8_t render_frame)
{
	std::vector<SDL_GPUStorageBufferReadWriteBinding> buffers;
	buffers.reserve(names.size());

    for (const char* name : names)
    {
        auto it = buffers_data.find(name);
        if (it != buffers_data.end())
        {
            SDL_GPUBuffer* buf = _GetGPUBufferForFrame(it->second.get(), render_frame);
            if (buf)
                buffers.push_back({ buf, false });
            else
                SDL_Log("Buffer '%s' is invalid (BindGPUComputeStorageBuffers)", name);
        }
        else {
            SDL_Log("BindGPUComputeStorageBuffers::Buffer '%s' not found", name);
        }
	}
	return buffers;
}

void BufferManager::BindGPUComputeRO_Buffers(SDL_GPUComputePass* cmp, uint32_t slot, const std::vector<BufferData*>& buffers_data, uint8_t frame)
{
    std::vector<SDL_GPUBuffer*> buffers;
    buffers.reserve(buffers_data.size());
    for (const BufferData* data : buffers_data) {
        SDL_GPUBuffer* buf = _GetGPUBufferForFrame(data, frame);
        if (buf) buffers.push_back(buf);
        else SDL_Log("Null buffer in BindGPUComputeROStorageBuffers");
    }

    SDL_BindGPUComputeStorageBuffers(cmp, slot, buffers.data(), safe_u32(buffers.size()));
}