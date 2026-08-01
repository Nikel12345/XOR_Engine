#pragma once
#include <SDL3/SDL_gpu.h>
#include <vector>
#include <memory>
#include <string>

// Конвенция упаковки ИСХОДНОГО файла текстуры. Канон движка: G = linear roughness (ORM),
// A нормал-карты = HEIGHT (яркое = выше; POM марчит depth = 1 - A).
// Импорт нормализует к канону один раз на CPU (см. EngineContext::CreateTextureFromFile),
// поэтому шейдер/материалы всегда видят единую конвенцию — без рантайм-веток и флагов на материале.
// Индексы каналов одинаковы в RGBA и BGRA (G = 1, A = 3), так что инверсии формат-независимы.
enum class ChannelConvention {
	AsIs,                // без изменений (по умолчанию) — файл уже в каноне движка
	SmoothnessInGreen,   // G = smoothness/glossiness (Unity-стиль) → инвертируется в roughness (G = 255 - G)
	DepthInAlpha,        // A = depth/cavity (яркое = ГЛУБЖЕ, напр. wood_normal_h) → инвертируется в height (A = 255 - A)
};

struct TextureData {
	uint32_t uv_packed_offset;  // unorm16 × 2: offset_x в low, offset_y в high
	uint32_t uv_packed_scale;   // unorm16 × 2: scale_x в low, scale_y в high
	uint32_t layer;
	uint32_t _pad;
};

struct TextureAtlas{
	// НЕвладеющий список размещений текстур в атласе. Держим именно TextureData (а не TextureHandle):
	// атлас — тонкая прослойка над GPU-текстурой, ему нужны только регионы (UVL+layer). Владелец
	// самих TextureData — TextureManager::handles_data (по значению внутри TextureHandle, адрес
	// стабилен). Пусто для атласов-таргетов, где хэндлы не создавались. Регион каждого элемента
	// точно распаковывается из UVL (unorm16, пиксель-в-пиксель) — этого достаточно для пересборки
	// свободного места при удалении и для гипотетической GPU→GPU перепаковки.
	std::vector<TextureData*> textures;
	// ВНИМАНИЕ: .texture может быть nullptr до бейка (TextureManager::BakeGPUResources) — обёртка
	// живёт с момента CreateTextureAtlas, GPU-текстура создаётся позже. Поэтому нигде нельзя
	// КОПИРОВАТЬ этот биндинг на этапе setup: держи TextureAtlas* и резолви на исполнении
	// (так делают проходы, блиты, compute-биндинги и слепок батчей).
	SDL_GPUTextureSamplerBinding texture_binding;
	// Полное описание для (пере)создания GPU-текстуры — источник истины об usage-флагах и размерах.
	// Нужен и бейку (отложенное создание), и ресайзу (пересоздание под новый размер окна).
	// tci.usage — ЕДИНОЕ поле флагов (ручной раздачи больше нет): GPU-текстуру бейк создаёт ровно
	// по тому, что в нём накопилось к этому моменту. Источники:
	//
	//   НАМЕРЕНИЕ — остаётся в tci.usage при создании (CreateTextureAtlas стрижёт всё остальное):
	//     SAMPLER из пресета. Граф его вывести НЕ МОЖЕТ: атлас заводится пустым, текстуры и
	//     материалы приезжают позже (другая сцена, рантайм), а GPU-текстура после создания
	//     неизменяема — дождёшься фактического использования и получишь атлас без SAMPLER, который
	//     сломается при первой же загрузке в него текстуры. Пустой атлас-под-материалы и пустой
	//     атлас-таргет по факту неразличимы, поэтому цель заявляет ПРЕСЕТ (AlbedoAtlas/ORMAtlas/…).
	//     Плюс самовыводимое: num_levels > 1 → SAMPLER | COLOR_TARGET (мип-ген требует ОБА —
	//     SDL_gpu.c:2498, проверено зондом).
	//
	//   ДЕКЛАРАЦИИ — доливаются там, где на атлас сослались (все обязаны случиться ДО бейка):
	//     SetColorTexture / SetDepthTexture прохода → COLOR_TARGET / DEPTH_STENCIL_TARGET
	//     RenderPassStep::SetGlobalTextures         → SAMPLER
	//     MaterialManager::CollectSamplerUsage      → SAMPLER (слот материала = фрагментный сэмплер)
	//     BatchBuilder::SetDummyTexture             → SAMPLER (фолбэк подставляется в слот)
	//     CreateComputeShaderProgram: ro → COMPUTE_STORAGE_READ, rw → COMPUTE_STORAGE_WRITE,
	//                                 сэмплеры → SAMPLER, need_simultaneous → SIMULTANEOUS_READ_WRITE
	//     CreateBlitPass: src → SAMPLER, dst → COLOR_TARGET (SDL требует ровно это — зонд)
	//
	// Слияние — чистый union. Опоздавшая декларация (атлас уже создан) флаг на GPU не изменит —
	// детекторы USAGE VIOLATION называют виновника, иначе SDL абортит анонимно.
	SDL_GPUTextureCreateInfo tci{};
	std::string debug_name;   // = ключ в atlases_data; нужен диагностике (назвать атлас в логе)
	// != nullptr — атлас ДЕЛИТ GPU-текстуру с другим (CreateTextureAtlas от existing_atlas):
	// своей не создаёт, на бейке копирует чужую. Владелец текстуры — источник.
	TextureAtlas* shares_with = nullptr;
	SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_INVALID;
	SDL_GPUTextureType texture_type = SDL_GPU_TEXTURETYPE_2D; // 2D / CUBE / ARRAY — берётся из tci при создании; компатибилити-проверки (это куб?) смотрят сюда
	uint32_t width = 0;
	uint32_t height = 0;
	uint16_t layers = 0;
	uint8_t padding = 0; // Рамка-gutter вокруг тайла; ставится в CreateTextureAtlas (мип → 1, иначе 0)
	uint8_t mip_levels = 1;
};

//struct TextureData {
//	SDL_GPUTextureSamplerBinding texture;
//	uint32_t w = 0;
//	uint32_t h = 0;
//};


// enable_shared_from_this: владелец — TextureManager::handles_data (shared_ptr). Материалы держат
// weak_ptr на хэндл (см. Material::textures) и потому умеют детектировать его удаление. weak_from_this()
// позволяет выдать такой weak_ptr из сырого TextureHandle* без протаскивания shared_ptr через API.
struct TextureHandle : std::enable_shared_from_this<TextureHandle> {
	TextureAtlas* atlas = nullptr;
	TextureData texture_data{};   // GPU-раскладка UVL, ПО ЗНАЧЕНИЮ: handle владеет своей записью,
	                              // адрес стабилен (handles_data держит shared_ptr<TextureHandle>).
	uint32_t width = 0;   // нативный размер картинки в пикселях, пишется при загрузке
	uint32_t height = 0;  // (точный исходник, без round-trip через unorm16 uv_packed_scale)

	// Авторские данные (для редактора/сериализации): ресурс самоописываем — знает, откуда и как
	// загружен, чтобы форма правки заполнялась, а сцена могла его сохранить и переинициализировать.
	// source_path ПУСТ у текстур из сырых пикселей (сгенерированы кодом) — из файла не пересоздаются.
	std::string       atlas_name;
	std::string       source_path;
	ChannelConvention conv = ChannelConvention::AsIs;
	// Непусто ТОЛЬКО у f0-грани кубмапы: логическое имя куба (без суффикса "_f0") для сериализации.
	// SaveScene пишет одну запись с этим именем и "cube": true; LoadScene пересоздаёт все 6 граней
	// через CreateCubeMapTexture. Грани f1..f5 остаются без source_path — сами по себе не сохраняются.
	std::string       cube_name;
	// Не писать в файл сцены при SaveScene. Движковые дефолты (_NoTextureDummy, default_*)
	// ставят true — они гарантированно пересоздаются кодом до всякой загрузки, файл ими не
	// раздуваем. UI-пересоздание идёт через create-API с дефолтом false → «тронул = сохраняемый».
	bool              dont_save = false;
	// Превью для UI держит отдельная подсистема PreviewPacker (ключ — ИМЯ текстуры), поэтому хэндл
	// про превью ничего не знает — он только про GPU-раскладку. См. TextureManager::preview.
};

class TextureManager;

// Depth-таргет — это ОБЫЧНЫЙ TextureAtlas (как shadow-depth): создаётся CreateTextureAtlas с
// depth-tci и nullptr-сэмплером (depth основного прохода не сэмплится), usage DEPTH_STENCIL_TARGET
// доложит декларация SetDepthTexture, ресайз — штатной инструкцией (RecreateAtlasTexture). Отдельного
// типа под depth больше нет — свопчейн особый (внешняя текстура от SDL), а depth мы владеем сами.