#pragma once
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <vector>
#include <memory>
#include <mutex>

static constexpr uint32_t BASE_TB_SIZE = 10 * 1024 * 1024;

// Запись пула transfer-буферов. Acquire*TB() выдаёт её загрузчику целиком на одну фазу;
// внутри фазы загрузчик сам линейно суб-аллоцирует буфер через tb_offset'ы своих тасков.
struct TransferBufferData {
	SDL_GPUTransferBuffer* tb = nullptr;
	void* mapped = nullptr;   // замаплен на время аренды (Acquire → Release)
	uint32_t size = 0;
	bool busy = false;
};

// Пул transfer-буферов, общий для всех загрузчиков (BufferManager, TextureManager, ...).
// Acquire атомарно выдаёт свободный TB подходящей ёмкости (или создаёт новый и добавляет
// в пул), Release возвращает его. Пул только растёт — стабилизируется на пиковой нагрузке.
class TransferManager
{
public:
	TransferManager(SDL_GPUDevice* device);
	~TransferManager();

	// Выдаёт замапленный TB ёмкостью >= size, помеченный занятым. size == 0 → nullptr.
	TransferBufferData* AcquireUploadTB(uint32_t size);
	TransferBufferData* AcquireDownloadTB(uint32_t size);

	// КОНТРАКТ: вызывающий гарантирует, что GPU закончил работу с буфером — fence фазы,
	// в которой буфер использовался, дождались. ReleaseTB(nullptr) — допустимый no-op
	// (пустая фаза).
	// CONTRACT: the caller guarantees the GPU is done with the buffer — the fence of the
	// phase that used it has been waited on. ReleaseTB(nullptr) is a valid no-op.
	void ReleaseTB(TransferBufferData* tbd);

private:
	TransferBufferData* AcquireTB(std::vector<std::unique_ptr<TransferBufferData>>& pool, uint32_t size, SDL_GPUTransferBufferUsage usage);
	// Ищет наименьшую свободную запись ёмкостью >= size (класс размера: BASE_TB_SIZE,
	// удваиваемый) или создаёт новую, помечает busy и возвращает. Лочит mtx сам, только на
	// скан/push_back; создание буфера идёт вне лока.
	TransferBufferData* EnsureTBCapacity(std::vector<std::unique_ptr<TransferBufferData>>& pool, uint32_t size, SDL_GPUTransferBufferUsage usage);

	SDL_GPUDevice* dev = nullptr;
	// unique_ptr — адреса записей стабильны при росте пула: указатели на руках у загрузчиков.
	std::vector<std::unique_ptr<TransferBufferData>> upload_pool;
	std::vector<std::unique_ptr<TransferBufferData>> download_pool;
	// Защищает только скан/рост пула и флаги busy; работа с выданным буфером идёт без лока.
	std::mutex mtx;
};
