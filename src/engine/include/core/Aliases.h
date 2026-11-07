#include <string>
#include <cstdint>
#include <vector>

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
using UIVarKey = std::vector<uint32_t>;

using BufferDataName = const char*;

namespace BatchKeys {
	using CommandBatchKey = uint64_t;
	using TextureBatchKey = uint64_t;
	using GpuResourceBatchKey = uint64_t;
	using ShaderBatchKey = uint64_t;
	using MatSpKey = uint64_t;
	using InstanceGroupKey = uint64_t;
};