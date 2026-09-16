#include "PCH.h"
#include "DefaultResourceSet.h"
#include "EngineContext.h"
#include "TextureManager.h"
#include "TextureData.h"
#include "BatchBuilder.h"
#include "TexturesPresets.h"
#include "PositionStructure.h"

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
				// U зеркалим (1-u): без этого надпись читалась ЗЕРКАЛЬНО (только изнутри сферы). V уже
				// v-down (v=0 у полюса φ=0 = верх картинки) — канон, не трогаем. Тангенс — вдоль НОВОГО
				// +U (∂pos/∂(−θ)) → знак θ-производной инвертируется, чтобы TBN совпал с cross(T,N).
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
