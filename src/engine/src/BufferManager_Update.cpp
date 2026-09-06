#include "PCH.h"
#include "BufferManager.h"
#include "EngineProfiler.h"

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

TransferBufferData* BufferManager::ExecutePrePassUpdateInstruction(SDL_GPUCopyPass* cp)
{
    return _ExecuteUpdateInstructions(cp, prepass_update_instructions, prepass_upload_tasks);
}

TransferBufferData* BufferManager::ExecuteUpdateInstructions(SDL_GPUCopyPass* cp)
{
    return _ExecuteUpdateInstructions(cp, update_instructions, upload_tasks);
}

void BufferManager::ExecutePrePassUploadTasks(SDL_GPUCopyPass* cp, uint8_t idx)
{
    _ExecuteUploadTasks(cp, prepass_upload_tasks, idx);
}

void BufferManager::ExecuteUploadTasks(SDL_GPUCopyPass* cp, uint8_t idx) {
    _ExecuteUploadTasks(cp, upload_tasks, idx);
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

TransferBufferData* BufferManager::_ExecuteUpdateInstructions(SDL_GPUCopyPass* cp, std::vector<UpdateInstruction>& target_instr_vector, std::vector<UploadTask>& target_task_vector)
{
    target_task_vector.clear();
    target_task_vector.reserve(target_instr_vector.size());

    // ── ПРОФАЙЛ: фаза 1 — «замер размера» (size_fn) по каждому буферу отдельно.
    //    Метка "<buf> .size", плюс сам вычисленный размер в байтах (столбец KB).
    const uint8_t li = logic_index.load();
    for (auto& instr : target_instr_vector) {
        UploadTask task{};
        task.dst_buffer_data = instr.buffer_data;
        // Гейт незабейканного буфера: GPU-хэндла нет (ни одна SP не объявила usage → BakePending его
        // не создал; напр. UI-текст-буферы в игре без UI-шейдера). Пропускаем инструкцию ЦЕЛИКОМ, а
        // не только финальную заливку (та и так гейтится в _ExecuteUploadTasks): незачем считать
        // size_fn, растить ёмкость, арендовать TB и гонять updater ради данных, которым некуда ехать.
        // size=0 держит выравнивание instr↔task для фазы 2 и глушит апдейт вниз по конвейеру.
        // Появится usage → BakePending создаст буфер, и обновлялка оживёт со следующего кадра сама.
        if (!_GetGPUBufferForFrame(instr.buffer_data, li)) {
            task.size = 0;
            target_task_vector.push_back(task);
            continue;
        }
        {
            // Push/Pop, а не Add: замер ОБЪЕМЛЕТ чужие скоупы (size_fn и updater могут
            // содержать свои PROF_SCOPE), и только открытый скоуп делает их детьми в
            // отчёте. С Add они оказались бы соседями, а [other] родителя - отрицательным.
            const char* nm = instr.buffer_data ? instr.buffer_data->debug_name.c_str() : "?";
            const size_t ph = Prof::Sim().Push((std::string(nm) + " .size").c_str());
            auto t = Prof::Clock::now();
            task.size = instr.size_fn ? instr.size_fn() : 0;
            Prof::Sim().Pop(ph, Prof::MsSince(t), task.size);
        }
        task.additional_offset = instr.offset_fn ? instr.offset_fn() : 0;
        if (!instr.updater) {
            task.resize_dst_buf_only = true;
        }
        target_task_vector.push_back(task);
    }

    // ── ПРОФАЙЛ: построение задач + аренда TB + EnsureBufferCapacity (пересоздание
    //    GPU-буферов при росте) — общая для всех буферов стадия.
    TransferBufferData* tbd;
    {
        auto t = Prof::Clock::now();
        tbd = _BuildUploadTasks(cp, target_task_vector);
        Prof::Sim().Add("  [build_tasks + ensure_capacity]", Prof::MsSince(t));
    }

    // ── ПРОФАЙЛ: фаза 2 — «запись» (updater пишет данные в transfer-буфер) по буферам.
    //    Метка "<buf> .store".
    for (size_t i = 0; i < target_instr_vector.size(); ++i) {
        auto& instr = target_instr_vector[i];
        auto& task = target_task_vector[i];
        if (instr.updater && task.size > 0) {
            const char* nm = instr.buffer_data ? instr.buffer_data->debug_name.c_str() : "?";
            const size_t ph = Prof::Sim().Push((std::string(nm) + " .store").c_str());
            auto t = Prof::Clock::now();
            instr.updater(cp, this, task);
            Prof::Sim().Pop(ph, Prof::MsSince(t));
        }
    }
    return tbd;
}
TransferBufferData* BufferManager::_BuildUploadTasks(SDL_GPUCopyPass* cp, std::vector<UploadTask>& target_vector)
{
    uint8_t li = logic_index.load();
    std::unordered_map<BufferData*, uint32_t> buffers_written;
    std::unordered_map<BufferData*, uint32_t> buffers_req_size;
    uint32_t tb_size = 0;

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
        task.tb_offset = tb_size;
        tb_size += task.size;
    }

    for (auto& [buffer_data, req_size] : buffers_req_size)
        EnsureBufferCapacity(cp, buffer_data, req_size, li);

    TransferBufferData* tbd = trm->AcquireUploadTB(tb_size);
    for (auto& task : target_vector)
        if (!task.resize_dst_buf_only)
            task.tbd = tbd;
    return tbd;
}

void BufferManager::_ExecuteUploadTasks(SDL_GPUCopyPass* cp, std::vector<UploadTask>& target_vector, uint8_t idx)
{
    for (auto task : target_vector) {
        if (task.size == 0 || task.resize_dst_buf_only || !task.tbd) continue;
        BufferData* buffer_data = task.dst_buffer_data;
        SDL_GPUBuffer* target_buffer = _GetGPUBufferForFrame(buffer_data, idx);
        // Буфер мог не забейкаться (ни одна SP не объявила usage → GPU-хэндла нет; напр. UI-текст-
        // буферы в игре без UI-шейдера). Заливать в null нельзя — SDL_UploadToGPUBuffer ассертит.
        // Обновлялка на незабейканный буфер = безвредный no-op (потребителя всё равно нет).
        if (!target_buffer) continue;
        SDL_GPUTransferBufferLocation src = { task.tbd->tb, task.tb_offset };
        SDL_GPUBufferRegion dstReg = { target_buffer, task.dst_offset, task.size };
        SDL_UploadToGPUBuffer(cp, &src, &dstReg, false);
    }
    target_vector.clear();
}

void* BufferManager::AcquireTransferWritePtr(UploadTask* task, Uint32 size)
{
    if (!task->tbd || !task->tbd->mapped) {
        SDL_Log("AcquireTransferWritePtr: task has no mapped transfer buffer");
        return nullptr;
    }

    uint8_t li = logic_index.load();

    if (size > task->size - task->written_size) {
        SDL_Log("AcquireTransferWritePtr: size exceeds task size");
        return nullptr;
    }

    // Проверка границ буфера-назначения с учётом базового смещения (dst_offset уже его включает).
    // Destination-bounds check accounting for the base offset (dst_offset already includes it).
    const Uint32 dst_capacity = (task->dst_buffer_data->type == BufferDataType::Static)
        ? task->dst_buffer_data->Static.buffer_size
        : task->dst_buffer_data->Dynamic.buffer_size[li];
    if (task->dst_offset + task->written_size + size > dst_capacity) {
        SDL_Log("AcquireTransferWritePtr: write at offset %u + %u exceeds destination buffer '%s' capacity %u",
            task->dst_offset + task->written_size, size, task->dst_buffer_data->debug_name.c_str(), dst_capacity);
        return nullptr;
    }

    std::byte* dst = static_cast<std::byte*>(task->tbd->mapped) + task->tb_offset + task->written_size;
    task->written_size += size;

    // Использованный объём = базовое смещение + дозаписанное (dst_offset включает базу).
    // МАКСИМУМ, а не присваивание: с тех пор как база заливки может быть НЕ концом занятого
    // (аллокатор диапазонов сажает пачку в освободившуюся дыру), запись в начало буфера занизила
    // бы эту величину — а её берёт copy_size в EnsureBufferCapacity, и ближайший рост буфера
    // молча отрезал бы весь хвост. Смысл поля от этого — «высокая вода», а не «занято сейчас»;
    // единственный потребитель (RESIZE_AND_COPY) от лишних скопированных байт не страдает.
    // Used size = base offset + appended bytes (dst_offset includes the base). MAX, not assign:
    // an upload into a reclaimed hole would otherwise lower the high-water mark that
    // EnsureBufferCapacity copies on resize, silently truncating the buffer's tail.
    const Uint32 used = task->dst_offset + task->written_size;
    switch (task->dst_buffer_data->type) {
    case BufferDataType::Static:
        if (used > task->dst_buffer_data->Static.used_buffer_size)
            task->dst_buffer_data->Static.used_buffer_size = used;
        break;

    case BufferDataType::Dynamic:
        if (used > task->dst_buffer_data->Dynamic.used_buffer_size[li])
            task->dst_buffer_data->Dynamic.used_buffer_size[li] = used;
        break;
    }

    return dst;
}

void BufferManager::UploadToTransferBuffer(UploadTask* task, Uint32 size, const void* data)
{
    void* dst = AcquireTransferWritePtr(task, size);
    if (!dst) return;
    SDL_memcpy(dst, data, size);
}

TransferBufferData* BufferManager::ExecuteReadBackInstructionsSize()
{
    download_tasks.clear();
    download_tasks.reserve(readback_instructions.size());

    for (auto& instr : readback_instructions) {
        ReadBackTask task{};
        task.src_buffer_data = instr.buffer_data;
        task.size = instr.size_fn ? instr.size_fn() : 0;
        download_tasks.push_back(task);
    }

    return _BuildDownloadTasks();
}

TransferBufferData* BufferManager::_BuildDownloadTasks() {
    std::unordered_map<BufferData*, uint32_t> buffers_used_data_size;
    uint32_t tb_size = 0;

    for (auto& task : download_tasks) {
        task.tb_offset = tb_size;
        task.src_offset = buffers_used_data_size[task.src_buffer_data];
        buffers_used_data_size[task.src_buffer_data] += task.size;
        tb_size += task.size;
    }

    TransferBufferData* tbd = trm->AcquireDownloadTB(tb_size);
    for (auto& task : download_tasks)
        task.tbd = tbd;
    return tbd;
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

TransferBufferData* BufferManager::ExecutePostReadbackInstructions(SDL_GPUCopyPass* cp)
{
    return _ExecuteUpdateInstructions(cp, post_readback_update_instructions, post_readback_upload_tasks);
}

void BufferManager::ExecutePostreadBackUploadTasks(SDL_GPUCopyPass* cp, uint8_t idx)
{
    _ExecuteUploadTasks(cp, post_readback_upload_tasks, idx);
}

void BufferManager::ExecuteDownloadTasks(SDL_GPUCopyPass* cp, uint8_t idx) {
    for (auto task : download_tasks) {
        if (task.size == 0 || !task.tbd) continue;
        BufferData* buffer_data = task.src_buffer_data;
        SDL_GPUBuffer* source_buffer = _GetGPUBufferForFrame(buffer_data, idx);
        SDL_GPUBufferRegion srcReg = { source_buffer, task.src_offset, task.size };
        SDL_GPUTransferBufferLocation dst = { task.tbd->tb, task.tb_offset };

        SDL_DownloadFromGPUBuffer(cp, &srcReg, &dst);
    }
}

std::span<const std::byte> BufferManager::ReadFromTransferBuffer(ReadBackTask* task, uint32_t size)
{
    const std::byte* dummy = nullptr;
    if (!task->tbd || !task->tbd->mapped) {
        SDL_Log("ReadFromTransferBuffer: task has no mapped transfer buffer");
        return { dummy, 0 };
    }
    if (size > task->size - task->read_size) {
        SDL_Log("ReadFromTransferBuffer: size exceeds task size");
        return { dummy, 0 };
    }
    const std::byte* p = static_cast<const std::byte*>(task->tbd->mapped)
        + task->tb_offset + task->read_size;

    task->read_size += size;
    return { p, size };
}