#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include "TextureData.h"

// SDL_ttf держим ТОЛЬКО в .cpp: заголовок форвардит opaque-тип шрифта.
struct TTF_Font;
class TextureManager;
class BufferManager;
struct TextureAtlas;
struct UploadTask;

// Один растеризованный глиф в глиф-атласе. Всё об атлас-ячейке — в handle (единый источник):
// атлас = handle->atlas, UVL = handle->texture_data (валиден ПОСЛЕ PackAtlases), пиксельный
// размер = handle->width/height. Дублировать их тут не нужно.
struct GlyphInfo {
	uint32_t       codepoint = 0;
	TextureHandle* handle    = nullptr;   // владелец — TextureManager; nullptr у пустого глифа (пробел)
	int            advance   = 0;         // сдвиг пера после глифа, px (для пропорциональной раскладки)
};

// GPU-раскладка одной записи UI_FONT_UVL_BUFFER (в шейдере — uint4, см. ui/glyph_text.hlsli).
// Своя, а не общая с UVL_Block батча: четвёртое слово здесь ЗНАЧИМО — advance, нормированный на
// line_height, — и имя обязано это говорить. Общего у них только 12 байт UVL, и те приходят из
// TextureData; сводить их в один тип значит снова завести слово с двумя хозяевами.
struct alignas(16) GlyphUVL {
	uint32_t uv_packed_offset = 0;
	uint32_t uv_packed_scale  = 0;
	uint32_t layer            = 0;
	uint32_t advance_bits     = 0;   // биты float: advance / line_height
};
static_assert(sizeof(GlyphUVL) == 16, "GlyphUVL using in shader as uint4");

// Растеризованный шрифт: глиф-атлас + метрики + карта codepoint→glyph_index + GlyphUVL.
// Топ-левел (не вложен в FontManager) — чтобы EngineContext возвращал FontData* форвардом,
// как TextureHandle*/Material* (заголовок фасада не тянет FontManager.h).
struct FontData {
	std::string   name;
	TTF_Font*     ttf = nullptr;          // владелец — FontManager (TTF_CloseFont в его dtor)
	float         px  = 0.0f;
	bool          sdf = false;
	int           line_height = 0;
	TextureAtlas* atlas = nullptr;        // владелец — TextureManager

	std::vector<GlyphInfo>                  glyphs;        // индекс = glyph_index
	std::unordered_map<uint32_t, uint32_t>  cp_to_index;  // codepoint → glyph_index
};

// FontManager — растеризация шрифтов в глиф-атлас. РЕСУРСНЫЙ менеджер (не покадровый модуль:
// растеризация однократна, как загрузка текстуры/модели). НЕ владеет другими менеджерами —
// TextureManager/BufferManager приходят ПАРАМЕТРАМИ из EngineContext::CreateFont (см. CLAUDE.md).
// TTF_Init/Quit живут в его ctor/dtor.
class FontManager {
public:
	FontManager();
	~FontManager();

	// Растеризует шрифт в НОВЫЙ глиф-атлас. tm — чужой менеджер, приходит параметром (владение у
	// EngineContext, FM его не хранит). Набор кодпоинтов сейчас фикс: ASCII + кириллица (что есть в
	// шрифте — TTF_FontHasGlyph). Возвращает FontData* (владелец — FontManager) или nullptr.
	FontData* CreateFont(TextureManager* tm, const std::string& name, const char* path, float px, bool sdf = false);

	FontData* GetFont(const std::string& name);

	// glyph_index по кодпоинту (0xFFFFFFFF — нет глифа в шрифте).
	uint32_t GlyphIndex(const FontData* font, uint32_t codepoint) const;

	// UTF-8 строка → коды глифов (glyph_index) для UITextComponent.glyphs. Кодпоинты, которых нет
	// в шрифте, пропускаются. Продвинутый shaping (кернинг/лигатуры) — позже; сейчас codepoint→index.
	std::vector<uint32_t> ShapeString(const FontData* font, const std::string& utf8) const;

	// Размер GlyphUVL-буфера в байтах (для size-фазы UpdateInstruction при проводке).
	uint32_t GlyphUvlBytes(const FontData* font) const;
	// Залить GlyphUVL (code=glyph_index → handle->texture_data) в общий UI_FONT_UVL_BUFFER.
	// UVL читается ПРЯМО из хэндлов (единый источник) — ВАЛИДНО только ПОСЛЕ PackAtlases. Отдельной
	// CPU-копии нет. Заводится при проводке (UpdateInstruction на UI_FONT_UVL_BUFFER).
	void StoreGlyphUVL(FontData* font, BufferManager* bm, UploadTask* task);

private:
	std::unordered_map<std::string, FontData> fonts_;
	bool ttf_ready_ = false;
};
