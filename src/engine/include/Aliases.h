#include <string>
#include <cstdint>

using RenderPassName = std::string;
using ComputePassName = std::string;
using ComputePrepassName = std::string;
using BlitPassName = std::string;
using SceneName = std::string;
using MaterialName = std::string;
using ModelName = std::string;
using AtlasName = std::string;
using TextureName = std::string;
using ShaderName = std::string;

using BufferDataName = const char*;

namespace BatchKeys {
	using ModelBatchKey = uint64_t;
	using TextureBatchKey = uint64_t;
	using AtlasBatchKey = uint64_t;
	using ShaderBatchKey = uint64_t;
	// Пара (материал, sp) — единица ПЕР-МАТЕРИАЛЬНОЙ половины батча (MatSpLayout): всё, что
	// texture-батч берёт из материала и не зависит от сущности. Ключ памятки предпрохода И
	// ресурсный вклад в TextureBatchKey (см. BatchBuilder: HashMatSpMemo/HashMatSpResources).
	using MatSpKey = uint64_t;
};