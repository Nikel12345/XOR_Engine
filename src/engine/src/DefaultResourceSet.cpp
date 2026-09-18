#include "PCH.h"
#include "DefaultResourceSet.h"
#include "EngineContext.h"
#include "TextureManager.h"
#include "TextureSamplerPresets.h"
#include "BufferManager.h"
#include "CameraStruct.h"
#include "LightStruct.h"
#include "TextureData.h"
#include "BatchBuilder.h"
#include "TexturesPresets.h"
#include "PositionStructure.h"

void DefaultResourceSet::CreateDefaultBuffers(BufferManager* bm)
{
    using namespace DefaultBuffersNames;
	bm->CreateBufferData(DEFAULT_TRANSFORM_BUFFER, BASE_TB_SIZE / 10, BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default | ResourceTag::System);
	bm->CreateBufferData(DEFAULT_LIGHT_BUFFER, sizeof(LightLayout) * 2, BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default | ResourceTag::System);
	bm->CreateBufferData(DEFAULT_CAMERA_BUFFER, sizeof(CameraData), BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default | ResourceTag::System);
	bm->CreateBufferData(DEFAULT_POSITION_INDEX_BUFFER, BASE_TB_SIZE / 16/ 10, BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default | ResourceTag::System);
	bm->CreateBufferData(DEFAULT_INSTANCE_BUFFER, BASE_TB_SIZE / 80, BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default | ResourceTag::System);
	bm->CreateBufferData(DEFAULT_LIGHT_CAMERA_BUFFER, sizeof(CameraData) * 6, BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default);

	bm->CreateBufferData(DEFAULT_TEX_STATE_RANK_BUFFER, sizeof(uint32_t) * 2 * 256, BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default | ResourceTag::System);
	bm->CreateBufferData(DEFAULT_TEX_STATE_INDEX_BUFFER, sizeof(uint32_t) * 256, BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default | ResourceTag::System);
	bm->CreateBufferData(DEFAULT_TEX_STATE_BUFFER, sizeof(uint32_t) * 256, BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default | ResourceTag::System);

    bm->CreateBufferData(DEFAULT_INDIRECT_BUFFER, sizeof(SDL_GPUIndexedIndirectDrawCommand) * 10, BufferDataType::Dynamic)
        ->usage |= SDL_GPU_BUFFERUSAGE_INDIRECT;

    bm->CreateBufferData(DEFAULT_BOUND_SPHERE_BUFFER, BASE_TB_SIZE / 40, BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default);
    bm->CreateBufferData(DEFAULT_OUT_PIB_BUFFER, BASE_TB_SIZE / 16 / 10, BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default);
    bm->CreateBufferData(DEFAULT_ENTITY_TO_CMD_BUFFER, BASE_TB_SIZE / 16 / 10, BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default);

    bm->CreateBufferData(UI_TEXT_RANK_BUFFER,     sizeof(uint32_t) * 2 * 64,  BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default);
    bm->CreateBufferData(UI_TEXT_INDEX_BUFFER,    sizeof(uint32_t) * 2 * 256, BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default);
    bm->CreateBufferData(UI_TEXT_BUFFER,          sizeof(uint32_t) * 4096,    BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default);

    bm->CreateBufferData(UI_FONT_UVL_BUFFER, sizeof(uint32_t) * 4 * 256, BufferDataType::Dynamic, ResizeBehaviour::RESIZE_ONLY, ResourceTag::Default | ResourceTag::System);
}

void DefaultResourceSet::CreateDefaultTextureResources(TextureManager* tm)
{
    using namespace DefaultSamplersNames;
    // У материального сэмплера анизотропия выключена намеренно: она выбирает LOD по резкой оси
    // футпринта и держит высокочастотную нормаль острой, сводя на нет мип-префильтр, из-за чего
    // шейдинг нормали мерцает при движении камеры.
    tm->CreateSampler(DEFAULT_SAMPLER, SamplerPresets::GetSamplerCreateInfo(SamplerPreset::DEFAULT_SAMPLER));
    tm->CreateSampler(DEFAULT_SHADOW_SAMPLER, SamplerPresets::GetSamplerCreateInfo(SamplerPreset::SHADOW_SAMPLER));
	tm->CreateSampler(VSM_SAMPLER, SamplerPresets::GetSamplerCreateInfo(SamplerPreset::VSM_SAMPLER));
	tm->CreateSampler(ENV_SAMPLER, SamplerPresets::GetSamplerCreateInfo(SamplerPreset::ENV_SAMPLER));
    tm->CreateSampler(SIMPLE_SAMPLER, SamplerPresets::GetSamplerCreateInfo(SamplerPreset::SIMPLE_SAMPLER));

    {
        SDL_GPUTextureCreateInfo tci{};
        tci.type                 = SDL_GPU_TEXTURETYPE_2D_ARRAY;
        tci.format               = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
        tci.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        tci.width                = 2048;
        tci.height               = 2048;
        tci.layer_count_or_depth = 1;
        tci.num_levels           = 1;
        tci.sample_count         = SDL_GPU_SAMPLECOUNT_1;
        TextureAtlas* text_atlas = tm->CreateTextureAtlas(DefaultAtlasNames::TEXT_ATLAS, tci, tm->GetSampler("SimpleSampler"), ResourceTag::Default | ResourceTag::System);
        text_atlas->padding = 0;
    }
}

void DefaultResourceSet::SetDefaultResources(EngineContext* ctx)
{
	TextureManager* tm = ctx->GetTextureManager();

	ctx->CreateTextureAtlas("FallbackAtlas", TexturePresets::AlbedoAtlas(64, 1, 1), "SimpleSampler", ResourceTag::Default | ResourceTag::System);
	ctx->CreateTextureFromFile("NoTextureDummy", "FallbackAtlas", "../engine/textures/dummy.png",
		ChannelConvention::AsIs, ResourceTag::CodeOwned | ResourceTag::Default | ResourceTag::System);

	ctx->GetBatchBuilder()->SetDummyTexture("NoTextureDummy", tm);

	TextureHandle* def_tex[] = {
		tm->CreateTexture("default_albedo",   "FallbackAtlas", 4, 4, std::vector<std::byte>(4 * 4 * 4, std::byte{ 0xFF })),
		tm->CreateTexture("default_normal",   "FallbackAtlas", 4, 4, std::vector<std::byte>(4 * 4 * 4, std::byte{ 0x80 })),
		tm->CreateTexture("default_orm",      "FallbackAtlas", 2, 2, std::vector<std::byte>(2 * 2 * 4, std::byte{ 0xFF })),
		tm->CreateTexture("default_emissive", "FallbackAtlas", 2, 2, std::vector<std::byte>(2 * 2 * 4, std::byte{ 0xFF })),
	};
	for (TextureHandle* h : def_tex) if (h) h->tags = ResourceTag::CodeOwned | ResourceTag::Default;

	// КАНОН развёртки у всех трёх примитивов: начало текстуры top-left (как грузит SDL_GPU и как
	// рисует ImGui) → V идёт ВНИЗ, v=0 у геометрического ВЕРХА. Развёртка при этом левосторонняя
	// относительно нормали — компенсируется глобально одним cross(T,N) в main_pass.vert (не флаг).
	// НЕ возвращай v-up: это перевернёт ориентированные текстуры.
	ctx->CreateModel<PosUVNormal>("quad", [](std::vector<PosUVNormal>& v, std::vector<Uint32>& i) {
		v = {
			{ 0,0,0,  0,1,  0,0,1,  1,0,0 },
			{ 1,0,0,  1,1,  0,0,1,  1,0,0 },
			{ 1,1,0,  1,0,  0,0,1,  1,0,0 },
			{ 0,1,0,  0,0,  0,0,1,  1,0,0 },
		};
		i = { 0, 1, 2, 0, 2, 3 };
	}, AnchorShift::Keep, ResourceTag::CodeOwned | ResourceTag::Default);

	ctx->CreateModel<PosUVNormal>("sphere", [](std::vector<PosUVNormal>& v, std::vector<Uint32>& idx) {
		const uint32_t stacks = 32;
		const uint32_t slices = 48;
		const float R = 1.0f;
		const float PI = 3.14159265358979323846f;

		for (uint32_t i = 0; i <= stacks; ++i) {
			float phi = PI * (float)i / (float)stacks;
			float cp = std::cos(phi), sp = std::sin(phi);
			for (uint32_t j = 0; j <= slices; ++j) {
				float theta = 2.0f * PI * (float)j / (float)slices;
				float ct = std::cos(theta), st = std::sin(theta);

				float nx = sp * ct, ny = cp, nz = sp * st;
				PosUVNormal vert{};
				vert.x = R * nx; vert.y = R * ny; vert.z = R * nz;
				vert.u = 1.0f - (float)j / (float)slices;
				vert.v = (float)i / (float)stacks;
				vert.nx = nx; vert.ny = ny; vert.nz = nz;
				vert.tx = st; vert.ty = 0.0f; vert.tz = -ct;
				v.push_back(vert);
			}
		}

		const uint32_t row = slices + 1;
		for (uint32_t i = 0; i < stacks; ++i) {
			for (uint32_t j = 0; j < slices; ++j) {
				uint32_t a = i * row + j;
				uint32_t b = a + row;
				idx.push_back(a);     idx.push_back(a + 1); idx.push_back(b);
				idx.push_back(a + 1); idx.push_back(b + 1); idx.push_back(b);
			}
		}
	}, AnchorShift::Keep, ResourceTag::CodeOwned | ResourceTag::Default);

	ctx->CreateModel<PosUVNormal>("cube", [](std::vector<PosUVNormal>& v, std::vector<Uint32>& idx) {
		struct FaceDef { float c[3], U[3], V[3], N[3]; };
		static const FaceDef faces[6] = {
			{{ 1,-1, 1}, { 0, 0,-2}, { 0, 2, 0}, { 1, 0, 0}},  // +X
			{{-1,-1,-1}, { 0, 0, 2}, { 0, 2, 0}, {-1, 0, 0}},  // -X
			{{-1, 1, 1}, { 2, 0, 0}, { 0, 0,-2}, { 0, 1, 0}},  // +Y
			{{-1,-1,-1}, { 2, 0, 0}, { 0, 0, 2}, { 0,-1, 0}},  // -Y
			{{-1,-1, 1}, { 2, 0, 0}, { 0, 2, 0}, { 0, 0, 1}},  // +Z
			{{ 1,-1,-1}, {-2, 0, 0}, { 0, 2, 0}, { 0, 0,-1}},  // -Z
		};
		const float uv[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };
		for (int f = 0; f < 6; ++f) {
			const FaceDef& fd = faces[f];
			float tx = fd.U[0], ty = fd.U[1], tz = fd.U[2];
			const float tl = std::sqrt(tx*tx + ty*ty + tz*tz);
			if (tl > 0.0f) { tx /= tl; ty /= tl; tz /= tl; }
			const uint32_t vbase = static_cast<uint32_t>(v.size());
			for (int q = 0; q < 4; ++q) {
				PosUVNormal vert{};
				vert.x = fd.c[0] + uv[q][0]*fd.U[0] + uv[q][1]*fd.V[0];
				vert.y = fd.c[1] + uv[q][0]*fd.U[1] + uv[q][1]*fd.V[1];
				vert.z = fd.c[2] + uv[q][0]*fd.U[2] + uv[q][1]*fd.V[2];
				vert.u = uv[q][0]; vert.v = 1.0f - uv[q][1];
				vert.nx = fd.N[0]; vert.ny = fd.N[1]; vert.nz = fd.N[2];
				vert.tx = tx;      vert.ty = ty;      vert.tz = tz;
				v.push_back(vert);
			}
			idx.push_back(vbase + 0); idx.push_back(vbase + 1); idx.push_back(vbase + 2);
			idx.push_back(vbase + 0); idx.push_back(vbase + 2); idx.push_back(vbase + 3);
		}
	}, AnchorShift::Keep, ResourceTag::CodeOwned | ResourceTag::Default);
}
