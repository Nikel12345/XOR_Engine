#pragma once
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <vector>
#include <memory>
#include <atomic>

static constexpr uint32_t BASE_TB_SIZE = 10 * 1024 * 1024;


struct TransferBufferData {
	SDL_GPUTransferBuffer* tb = nullptr;
	void* mapped = nullptr;   // замаплен на время аренды (Acquire → Release)
	uint32_t size = 0;
	bool busy = false;
	// Ставит стадия, дождавшаяся фенса; снимает аренда, возвращая запись в пул. Данные с флагом
	// не едут, поэтому relaxed: атомик нужен за определённое чтение с чужого потока.
	std::atomic<bool> returnable{ false };
};

// Пул transfer-буферов, общий для всех загрузчиков (BufferManager, TextureManager, ...).
// Acquire выдаёт свободный TB подходящей ёмкости (или создаёт новый и добавляет в пул),
// Release возвращает его. Пул только растёт — стабилизируется на пиковой нагрузке.
//
// ЗАМКОВ НЕТ: пулом распоряжается поток, который готовит кадр. Стадия, дождавшаяся фенса, только
// помечает запись через MarkReturnable, а возвращает её ближайшая аренда — на нужном потоке и по
// определению до того, как пул кому-то что-то выдаст.
class TransferManager
{
public:
	TransferManager(SDL_GPUDevice* device);
	~TransferManager();

	TransferBufferData* AcquireUploadTB(uint32_t size);
	TransferBufferData* AcquireDownloadTB(uint32_t size);
	void ReleaseTB(TransferBufferData* tbd);

	// Пометка «GPU дочитал, запись можно забирать» — для тех, кто ждёт фенс не на prep-потоке.
	// Сам возврат сделает ближайшая аренда. nullptr — no-op.
	void MarkReturnable(TransferBufferData* tbd);

private:
	TransferBufferData* AcquireTB(std::vector<std::unique_ptr<TransferBufferData>>& pool, uint32_t size, SDL_GPUTransferBufferUsage usage);
	void DrainReturnable(std::vector<std::unique_ptr<TransferBufferData>>& pool);
	TransferBufferData* EnsureTBCapacity(std::vector<std::unique_ptr<TransferBufferData>>& pool, uint32_t size, SDL_GPUTransferBufferUsage usage);

	SDL_GPUDevice* dev = nullptr;
	std::vector<std::unique_ptr<TransferBufferData>> upload_pool;
	std::vector<std::unique_ptr<TransferBufferData>> download_pool;
};
