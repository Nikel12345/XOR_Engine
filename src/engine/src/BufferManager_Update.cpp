#include "PCH.h"
#include "BufferManager.h"

void BufferManager::CreatePrePassUpdateInstruction(BufferData& buffer_data, UpdateInstructionUpdaterFunc fn = nullptr, UpdateInstructionSizeFunc size_fn = nullptr)
{
    _CreateUpdateInstruction(&buffer_data, prepass_update_instructions, fn, size_fn);
}

void BufferManager::CreatePrePassUpdateInstruction(BufferDataName name, UpdateInstructionUpdaterFunc fn = nullptr, UpdateInstructionSizeFunc size_fn = nullptr)
{
    BufferData* bd = this->GetBufferData(name);
    _CreateUpdateInstruction(bd, prepass_update_instructions, fn, size_fn);
}

void BufferManager::CreateUpdateInstruction(BufferData& buffer_data, UpdateInstructionUpdaterFunc fn = nullptr, UpdateInstructionSizeFunc size_fn = nullptr, UpdateInstructionOffsetFunc offset_fn)
{
    _CreateUpdateInstruction(&buffer_data, update_instructions, fn, size_fn, offset_fn);
}

void BufferManager::CreateUpdateInstruction(BufferDataName name, UpdateInstructionUpdaterFunc fn = nullptr, UpdateInstructionSizeFunc size_fn = nullptr, UpdateInstructionOffsetFunc offset_fn)
{
    BufferData* bd = this->GetBufferData(name);
    _CreateUpdateInstruction(bd, update_instructions, fn, size_fn, offset_fn);
}

void BufferManager::CreateReadBackInstruction(BufferData& buffer_data, ReadBackInstructionReaderFunc fn = nullptr, ReadBackInstructionSizeFunc size_fn = nullptr)
{
    ReadBackInstruction instr;
    instr.buffer_data = &buffer_data;
    instr.reader = std::move(fn);
    instr.size_fn = std::move(size_fn);
    readback_instructions.push_back(std::move(instr));
}

void BufferManager::CreateReadBackInstruction(BufferDataName name, ReadBackInstructionReaderFunc fn = nullptr, ReadBackInstructionSizeFunc size_fn = nullptr)
{
    BufferData* bd = this->GetBufferData(name);
    CreateReadBackInstruction(*bd, std::move(fn), std::move(size_fn));
}

void BufferManager::CreatePostReadbackUpdateInstruction(BufferData& buffer_data, UpdateInstructionUpdaterFunc fn, UpdateInstructionSizeFunc size_fn)
{
    _CreateUpdateInstruction(&buffer_data, post_readback_update_instructions, fn, size_fn);
}

void BufferManager::CreatePostReadbackUpdateInstruction(BufferDataName name, UpdateInstructionUpdaterFunc fn, UpdateInstructionSizeFunc size_fn)
{
    BufferData* bd = this->GetBufferData(name);
    CreatePostReadbackUpdateInstruction(*bd, fn, size_fn);
}

void BufferManager::ExecutePrePassUpdateInstruction(SDL_GPUCopyPass* cp)
{
    _ExecuteUpdateInstructions(cp, prepass_update_instructions, prepass_upload_tasks,
        current_prepass_dep_tb_offset,
        [this](uint32_t size) { EnsurePrepassDependedTransferBufferCapacity(size); });
}

void BufferManager::ExecuteUpdateInstructions(SDL_GPUCopyPass* cp)
{
    _ExecuteUpdateInstructions(cp, update_instructions, upload_tasks,
        current_upload_tb_offset,
        [this](uint32_t size) { EnsureUploadTransferBufferCapacity(size); });
}

void BufferManager::ExecutePrePassUploadTasks(SDL_GPUCopyPass* cp, uint8_t idx)
{
    _ExecuteUploadTasks(cp, prepass_upload_tasks, prepass_dep_transfer_buffer, idx);
}

void BufferManager::ExecuteUploadTasks(SDL_GPUCopyPass* cp, uint8_t idx) {
    _ExecuteUploadTasks(cp, upload_tasks, upload_transfer_buffer, idx);
}

void BufferManager::_CreateUpdateInstruction(BufferData* buffer_data, std::vector<UpdateInstruction>& target_vector, UpdateInstructionUpdaterFunc fn, UpdateInstructionSizeFunc size_fn, UpdateInstructionOffsetFunc offset_fn)
{
    UpdateInstruction instr;
    instr.buffer_data = buffer_data;
    instr.updater = std::move(fn);
    instr.size_fn = std::move(size_fn);
    instr.offset_fn = std::move(offset_fn);
    target_vector.push_back(std::move(instr));
}

void BufferManager::_ExecuteUpdateInstructions(SDL_GPUCopyPass* cp, std::vector<UpdateInstruction> target_instr_vector, std::vector<UploadTask>& target_task_vector, uint32_t& current_tb_offset, 
    std::function<void(uint32_t)> ensure_capacity_fn)
{
    target_task_vector.clear();
    target_task_vector.reserve(target_instr_vector.size());

    for (auto& instr : target_instr_vector) {
        UploadTask task{};
        task.dst_buffer_data = instr.buffer_data;
        task.size = instr.size_fn ? instr.size_fn() : 0;
        task.additional_offset = instr.offset_fn ? instr.offset_fn() : 0;
        if (!instr.updater) {
            task.resize_dst_buf_only = true;
        }
        target_task_vector.push_back(task);
    }

    _BuildUploadTasks(cp, target_task_vector, current_tb_offset, ensure_capacity_fn);

    for (size_t i = 0; i < target_instr_vector.size(); ++i) {
        auto& instr = target_instr_vector[i];
        auto& task = target_task_vector[i];
        if (instr.updater && task.size > 0) {
            instr.updater(cp, this, task);
        }
    }
}
void BufferManager::_BuildUploadTasks(SDL_GPUCopyPass* cp, std::vector<UploadTask>& target_vector, uint32_t& current_tb_offset, std::function<void(uint32_t)> ensure_capacity_fn)
{
    uint8_t li = logic_index.load();
    std::unordered_map<BufferData*, uint32_t> buffers_written;
    std::unordered_map<BufferData*, uint32_t> buffers_req_size;
    current_tb_offset = 0;

    for (auto& task : target_vector) {
        task.dst_offset = task.additional_offset + buffers_written[task.dst_buffer_data];
        uint32_t end = task.dst_offset + task.size;
        buffers_written[task.dst_buffer_data] += task.size;
        uint32_t& req = buffers_req_size[task.dst_buffer_data];
        if (end > req) req = end;

        if (task.resize_dst_buf_only) {
            task.tb_offset = 0;
            continue;
        }
        task.tb_offset = current_tb_offset;
        current_tb_offset += task.size;
    }

    for (auto& [buffer_data, req_size] : buffers_req_size)
        EnsureBufferCapacity(cp, buffer_data, req_size, li);

    ensure_capacity_fn(current_tb_offset);
}

void BufferManager::_ExecuteUploadTasks(SDL_GPUCopyPass* cp, std::vector<UploadTask>& target_vector, SDL_GPUTransferBuffer* target_tb, uint8_t idx)
{
    for (auto task : target_vector) {
        if (task.size == 0 || task.resize_dst_buf_only) continue;
        BufferData* buffer_data = task.dst_buffer_data;
        SDL_GPUBuffer* target_buffer = _GetGPUBufferForFrame(buffer_data, idx);
        SDL_GPUTransferBufferLocation src = { target_tb, task.tb_offset };
        SDL_GPUBufferRegion dstReg = { target_buffer, task.dst_offset, task.size };
        SDL_UploadToGPUBuffer(cp, &src, &dstReg, false);
    }
    target_vector.clear();
}

void BufferManager::UploadToPrePassTransferBuffer(UploadTask* task, Uint32 size, const void* data)
{
    _UploadToTransferBuffer(task, size, data, mapped_prepass_dep_tb);
}

void BufferManager::UploadToTransferBuffer(UploadTask* task, Uint32 size, const void* data) {
    _UploadToTransferBuffer(task, size, data, mapped_upload_tb);
}

void BufferManager::_UploadToTransferBuffer(UploadTask* task, Uint32 size, const void* data, void* target_mapped)
{
    if (!target_mapped) {
        SDL_Log("UploadToTransferBuffer called without mapping the upload transfer buffer");
        return;
    }

    uint8_t li = logic_index.load();

    if (size > task->size - task->written_size) {
        SDL_Log("UploadToTransferBuffer: size exceeds task size");
        return;
    }

    // Проверка границ буфера-назначения с учётом базового смещения (dst_offset уже его включает).
    // Destination-bounds check accounting for the base offset (dst_offset already includes it).
    const Uint32 dst_capacity = (task->dst_buffer_data->type == BufferDataType::Static)
        ? task->dst_buffer_data->Static.buffer_size
        : task->dst_buffer_data->Dynamic.buffer_size[li];
    if (task->dst_offset + task->written_size + size > dst_capacity) {
        SDL_Log("UploadToTransferBuffer: write at offset %u + %u exceeds destination buffer '%s' capacity %u",
            task->dst_offset + task->written_size, size, task->dst_buffer_data->debug_name.c_str(), dst_capacity);
        return;
    }

    std::byte* base = static_cast<std::byte*>(target_mapped);
    SDL_memcpy(base + task->tb_offset + task->written_size, data, size);
    task->written_size += size;

    // Использованный объём = базовое смещение + дозаписанное (dst_offset включает базу).
    // Used size = base offset + appended bytes (dst_offset includes the base).
    const Uint32 used = task->dst_offset + task->written_size;
    switch (task->dst_buffer_data->type) {
    case BufferDataType::Static:
        task->dst_buffer_data->Static.used_buffer_size = used;
        break;

    case BufferDataType::Dynamic:
        task->dst_buffer_data->Dynamic.used_buffer_size[li] = used;
        break;
    }
}

void BufferManager::ExecuteReadBackInstructionsSize()
{
    download_tasks.clear();
    download_tasks.reserve(readback_instructions.size());

    for (auto& instr : readback_instructions) {
        ReadBackTask task{};
        task.src_buffer_data = instr.buffer_data;
        task.size = instr.size_fn ? instr.size_fn() : 0;
        download_tasks.push_back(task);
    }

    _BuildDownloadTasks();
}

void BufferManager::_BuildDownloadTasks() {
    uint8_t li = logic_index.load();
    std::unordered_map<BufferData*, uint32_t> buffers_used_data_size;
    current_read_tb_offset = 0;

    for (auto& task : download_tasks) {
        task.tb_offset = current_read_tb_offset;
        task.src_offset = buffers_used_data_size[task.src_buffer_data];
        buffers_used_data_size[task.src_buffer_data] += task.size;
        current_read_tb_offset += task.size;
    }

    EnsureReadTransferBufferCapacity(current_read_tb_offset);
}

void BufferManager::ExecuteReadBackInstructionsReader()
{
    for (size_t i = 0; i < readback_instructions.size(); ++i) {
        auto& instr = readback_instructions[i];
        auto& task = download_tasks[i];
        if (instr.reader && task.size > 0) {
            instr.reader(this, task);
        }
    }
    download_tasks.clear();
}

void BufferManager::ExecutePostReadbackInstructions(SDL_GPUCopyPass* cp)
{
    _ExecuteUpdateInstructions(cp, post_readback_update_instructions, post_readback_upload_tasks, current_prepass_dep_tb_offset, [this](uint32_t size) {EnsurePrepassDependedTransferBufferCapacity(size); });
}

void BufferManager::ExecutePostreadBackUploadTasks(SDL_GPUCopyPass* cp, uint8_t idx)
{
    _ExecuteUploadTasks(cp, post_readback_upload_tasks, prepass_dep_transfer_buffer, idx);
}

void BufferManager::ExecuteDownloadTasks(SDL_GPUCopyPass* cp, uint8_t idx) {
    for (auto task : download_tasks) {
        if (task.size == 0) continue;
        BufferData* buffer_data = task.src_buffer_data;
        SDL_GPUBuffer* source_buffer = _GetGPUBufferForFrame(buffer_data, idx);
        SDL_GPUBufferRegion srcReg = { source_buffer, task.src_offset, task.size };
        SDL_GPUTransferBufferLocation dst = { read_transfer_buffer, task.tb_offset };
        //SDL_Log("Download source buffer ptr: %p", (void*)source_buffer);

        SDL_DownloadFromGPUBuffer(cp, &srcReg, &dst);
    }
}

std::span<const std::byte> BufferManager::ReadFromTransferBuffer(ReadBackTask* task, uint32_t size)
{
    const std::byte* dummy = nullptr;
    if (!mapped_read_tb) {
        SDL_Log("ReadFromTransferBuffer called without mapping the read transfer buffer");
        return { dummy, 0 };
    }
    if (size > task->size - task->read_size) {
        SDL_Log("ReadFromTransferBuffer: size exceeds task size");
        return { dummy, 0 };
    }
    const std::byte* p = static_cast<const std::byte*>(mapped_read_tb)
        + task->tb_offset + task->read_size;

    task->read_size += size;
    return { p, size };
}