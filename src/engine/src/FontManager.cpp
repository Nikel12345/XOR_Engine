#include "PCH.h"
#include "FontManager.h"
#include "Utils.h"
#include "TextureManager.h"
#include "BufferManager.h"
#include <SDL3_ttf/SDL_ttf.h>
#include <bit>

FontManager::FontManager()
{
	ttf_ready_ = TTF_Init();
	if (!ttf_ready_) SDL_Log("FontManager: TTF_Init failed: %s", SDL_GetError());
}

FontManager::~FontManager()
{
	for (auto& [n, fd] : fonts_)
		if (fd.ttf) TTF_CloseFont(fd.ttf);
	if (ttf_ready_) TTF_Quit();
}

FontData* FontManager::CreateFont(TextureManager* tm,
                                  const std::string& name, const char* path, float px, bool sdf)
{
	if (!ttf_ready_) { SDL_Log("FontManager::CreateFont: TTF not initialized"); return nullptr; }
	if (fonts_.count(name)) { SDL_Log("FontManager::CreateFont: '%s' already exists", name.c_str()); return &fonts_[name]; }

	TTF_Font* ttf = TTF_OpenFont(path, px);
	if (!ttf) { SDL_Log("FontManager::CreateFont: TTF_OpenFont('%s') failed: %s", path, SDL_GetError()); return nullptr; }
	if (sdf) TTF_SetFontSDF(ttf, true);   // SDF — resolution-independence; шейдер интерпретирует альфу как SDF

	FontData fd;
	fd.name = name;
	fd.ttf  = ttf;
	fd.px   = px;
	fd.sdf  = sdf;
	fd.line_height = TTF_GetFontHeight(ttf);

	// Единый текстовый атлас (создан заранее в конструкторе TM) — свой НЕ заводим. Глифы всех
	// шрифтов пакуются в него, packer даёт уникальные координаты.
	fd.atlas = tm->GetTextureAtlas(DefaultAtlasNames::TEXT_ATLAS);
	if (!fd.atlas) {
		SDL_Log("FontManager::CreateFont: text atlas '%s' not found", DefaultAtlasNames::TEXT_ATLAS);
		TTF_CloseFont(ttf);
		return nullptr;
	}

	// Растеризация диапазона кодпоинтов в глиф-атлас через штатный путь текстур: каждый глиф —
	// CreateTexture, PackAtlases (в кадре) посчитает ему UVL. Порядок = glyph_index.
	const SDL_Color WHITE{ 255, 255, 255, 255 };
	auto rasterize_range = [&](uint32_t lo, uint32_t hi) {
		for (uint32_t cp = lo; cp <= hi; ++cp) {
			if (!TTF_FontHasGlyph(ttf, cp)) continue;   // глиф — дело ШРИФТА, не библиотеки

			GlyphInfo g{};
			g.codepoint = cp;
			TTF_GetGlyphMetrics(ttf, cp, nullptr, nullptr, nullptr, nullptr, &g.advance);   // advance для пропорц. раскладки

			SDL_Surface* s = TTF_RenderGlyph_Blended(ttf, cp, WHITE);
			if (s && s->w > 0 && s->h > 0) {
				// Blended кладёт ПОКРЫТИЕ в АЛЬФУ (RGB=белый). Атлас R8 — вытаскиваем альфу в один
				// канал R. Пиксельный размер осядет в handle->width/height (свой в GlyphInfo не держим).
				std::vector<std::byte> bgra = TextureManager::SurfaceToPixels(s, SDL_PIXELFORMAT_BGRA32);
				if (!bgra.empty()) {
					const size_t n = static_cast<size_t>(s->w) * s->h;
					std::vector<std::byte> r8(n);
					for (size_t i = 0; i < n; ++i) r8[i] = bgra[i * 4 + 3];   // альфа BGRA (index 3) → R
					// имена глиф-текстур с "__" (движковая инфраструктура; в textures.json не пишутся)
					g.handle = tm->CreateTexture("__glyph_" + name + "_" + std::to_string(cp),
					                             fd.atlas, static_cast<uint32_t>(s->w), static_cast<uint32_t>(s->h),
					                             std::move(r8));
					if (g.handle) g.handle->dont_save = true;
				}
			}
			if (s) SDL_DestroySurface(s);   // пробел/пустой глиф: без пикселей, остаётся advance

			fd.cp_to_index[cp] = static_cast<uint32_t>(fd.glyphs.size());
			fd.glyphs.push_back(g);
		}
	};
	rasterize_range(0x0020, 0x007E);   // ASCII печатные
	rasterize_range(0x0400, 0x04FF);   // кириллица

	// GlyphUVL заливается в общий UI_FONT_UVL_BUFFER при проводке (StoreGlyphUVL читает UVL прямо
	// из хэндлов после PackAtlases — отдельной CPU-копии нет).

	auto [it, ok] = fonts_.emplace(name, std::move(fd));
	SDL_Log("FontManager: font '%s' rasterized - %zu glyphs into '%s'",
	        name.c_str(), it->second.glyphs.size(), DefaultAtlasNames::TEXT_ATLAS);
	return &it->second;
}

FontData* FontManager::GetFont(const std::string& name)
{
	auto it = fonts_.find(name);
	return it == fonts_.end() ? nullptr : &it->second;
}

uint32_t FontManager::GlyphIndex(const FontData* font, uint32_t codepoint) const
{
	if (!font) return 0xFFFFFFFFu;
	auto it = font->cp_to_index.find(codepoint);
	return it == font->cp_to_index.end() ? 0xFFFFFFFFu : it->second;
}

// Минимальный UTF-8 → кодпоинты (без валидации сверх необходимого): ведущий байт задаёт длину,
// продолжения 10xxxxxx подклеиваются. Бракованные последовательности пропускаются.
std::vector<uint32_t> FontManager::ShapeString(const FontData* font, const std::string& utf8) const
{
	std::vector<uint32_t> codes;
	if (!font) return codes;
	codes.reserve(utf8.size());

	for (size_t i = 0; i < utf8.size(); ) {
		const uint8_t c = static_cast<uint8_t>(utf8[i]);
		uint32_t cp; int len;
		if      (c < 0x80) { cp = c;          len = 1; }
		else if ((c >> 5) == 0x6) { cp = c & 0x1F; len = 2; }
		else if ((c >> 4) == 0xE) { cp = c & 0x0F; len = 3; }
		else if ((c >> 3) == 0x1E){ cp = c & 0x07; len = 4; }
		else { ++i; continue; }   // битый ведущий байт

		if (i + len > utf8.size()) break;
		bool ok = true;
		for (int k = 1; k < len; ++k) {
			const uint8_t cc = static_cast<uint8_t>(utf8[i + k]);
			if ((cc >> 6) != 0x2) { ok = false; break; }   // не продолжение 10xxxxxx
			cp = (cp << 6) | (cc & 0x3F);
		}
		i += len;
		if (!ok) continue;

		const uint32_t gi = GlyphIndex(font, cp);
		if (gi != 0xFFFFFFFFu) codes.push_back(gi);   // нет глифа в шрифте — пропуск
	}
	return codes;
}

uint32_t FontManager::GlyphUvlBytes(const FontData* font) const
{
	return font ? safe_u32(font->glyphs.size() * sizeof(GlyphUVL)) : 0u;
}

void FontManager::StoreGlyphUVL(FontData* font, BufferManager* bm, UploadTask* task)
{
	if (!font || font->glyphs.empty()) return;

	// UVL прямо из хэндлов (валидно после PackAtlases). advance НОРМИРУЕМ на line_height (биты
	// float) — FS суммирует их и получает пропорции ШРИФТА (ширина строки в единицах её высоты),
	// чтобы un-squish'ить текст под форму ректа без пикселей.
	// Пустой глиф (пробел): UVL нулевой, но advance есть → корректный пробел.
	const float inv_lh = font->line_height > 0 ? 1.0f / static_cast<float>(font->line_height) : 0.0f;
	for (const GlyphInfo& g : font->glyphs) {
		const TextureData td = g.handle ? g.handle->texture_data : TextureData{};
		GlyphUVL entry{ td.uv_packed_offset, td.uv_packed_scale, td.layer,
		                std::bit_cast<uint32_t>(static_cast<float>(g.advance) * inv_lh) };
		bm->UploadToTransferBuffer(task, sizeof(GlyphUVL), &entry);
	}
}
