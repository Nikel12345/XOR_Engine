#include "PCH.h"
#include "Engine.h"
#include "EngineProfiler.h"
// Engine.h теперь только forward-декларации — полные типы тянет этот TU.
#include "ObjectManager.h"
#include "TextureManager.h"
#include "ModelManager.h"
#include "MaterialManager.h"
#include "MaterialParamsSpec.h"
#include "ShaderManager.h"
#include "PipeManager.h"
#include "RenderManager.h"
#include "BufferManager.h"
#include "PositionStructure.h"
#include "BatchBuilder.h"
#include "EngineContext.h"
#include "UI_Yoga.h"   // MarkSceneReset после ECS-swap (UI-энтити производны от дерева)
#include <fstream>
#include <filesystem>
#include "yyjson.h"

using namespace ShaderBase;   // VertexSemantic/POSITION/VertexBufferBinding (в хедере using убран намеренно)

//  Сцена-папка: scene.scene (ECS) + файлы ресурсов рядом (json, по менеджерам — поэтапно).
//  Публичная точка входа — ctx->Save/LoadScene (тонкий прокси сюда).

// ChannelConvention ↔ строка манифеста (в json — читаемое имя, не число).
static const char* ConvToStr(ChannelConvention c)
{
	switch (c) {
	case ChannelConvention::SmoothnessInGreen: return "SmoothnessInGreen";
	case ChannelConvention::DepthInAlpha:      return "DepthInAlpha";
	default:                                   return "AsIs";
	}
}
static ChannelConvention ConvFromStr(const char* s)
{
	if (s && std::strcmp(s, "SmoothnessInGreen") == 0) return ChannelConvention::SmoothnessInGreen;
	if (s && std::strcmp(s, "DepthInAlpha") == 0)      return ChannelConvention::DepthInAlpha;
	return ChannelConvention::AsIs;
}
// Строковое поле json-объекта; отсутствие/не-строка → пустая строка (валидность решает потребитель).
static const char* JsonStr(yyjson_val* obj, const char* key)
{
	const char* s = yyjson_get_str(yyjson_obj_get(obj, key));
	return s ? s : "";
}
static bool JsonBool(yyjson_val* obj, const char* key, bool dflt) {
	yyjson_val* v = yyjson_obj_get(obj, key);
	return v ? yyjson_get_bool(v) : dflt;
}
static int JsonInt(yyjson_val* obj, const char* key, int dflt) {
	yyjson_val* v = yyjson_obj_get(obj, key);
	return v ? (int)yyjson_get_int(v) : dflt;
}
static double JsonReal(yyjson_val* obj, const char* key, double dflt) {
	yyjson_val* v = yyjson_obj_get(obj, key);
	return v ? yyjson_get_real(v) : dflt;
}

// VertexSemantic ↔ строка (набор фиксирован; реестра раскладок пока нет — как в UI VsdEditor).
static const char* SemToStr(VertexSemantic s) {
	switch (s) {
	case UV:      return "UV";
	case NORMAL:  return "NORMAL";
	case TANGENT: return "TANGENT";
	default:      return "POSITION";
	}
}
static VertexSemantic SemFromStr(const char* s) {
	if (s && std::strcmp(s, "UV") == 0)      return UV;
	if (s && std::strcmp(s, "NORMAL") == 0)  return NORMAL;
	if (s && std::strcmp(s, "TANGENT") == 0) return TANGENT;
	return POSITION;
}

// TextureSlotRole ↔ строка (взаимоисключающие роли sp). ORM==MetallicRoughness (алиас), Custom0=1000
// — поэтому round-trip строкой, а не числом.
static const char* RoleToStr(TextureSlotRole r) {
	switch (r) {
	case TextureSlotRole::Albedo:   return "Albedo";
	case TextureSlotRole::Normal:   return "Normal";
	case TextureSlotRole::ORM:      return "ORM";
	case TextureSlotRole::Emissive: return "Emissive";
	case TextureSlotRole::Custom0:  return "Custom0";
	case TextureSlotRole::Custom1:  return "Custom1";
	case TextureSlotRole::Custom2:  return "Custom2";
	case TextureSlotRole::Custom3:  return "Custom3";
	case TextureSlotRole::Custom4:  return "Custom4";
	case TextureSlotRole::Custom5:  return "Custom5";
	case TextureSlotRole::Custom6:  return "Custom6";
	case TextureSlotRole::Custom7:  return "Custom7";
	default:                        return "Albedo";
	}
}
static bool RoleFromStr(const char* s, TextureSlotRole& out) {
	if (!s) return false;
	static const std::pair<const char*, TextureSlotRole> kMap[] = {
		{ "Albedo", TextureSlotRole::Albedo }, { "Normal", TextureSlotRole::Normal },
		{ "ORM", TextureSlotRole::ORM }, { "Emissive", TextureSlotRole::Emissive },
		{ "Custom0", TextureSlotRole::Custom0 }, { "Custom1", TextureSlotRole::Custom1 },
		{ "Custom2", TextureSlotRole::Custom2 }, { "Custom3", TextureSlotRole::Custom3 },
		{ "Custom4", TextureSlotRole::Custom4 }, { "Custom5", TextureSlotRole::Custom5 },
		{ "Custom6", TextureSlotRole::Custom6 }, { "Custom7", TextureSlotRole::Custom7 },
	};
	for (auto& [n, role] : kMap) if (std::strcmp(s, n) == 0) { out = role; return true; }
	return false;
}

// spd ↔ json-объект. spd-подэнумы (cull/fill/primitive) — числа (SDL_GPU*-энумы, стабильны и
// малоинформативны строкой); тумблеры/биас — bool/real.
static void WriteSpd(yyjson_mut_doc* doc, yyjson_mut_val* obj, const ShaderProgramDescription& d) {
	yyjson_mut_obj_add_int (doc, obj, "cull_mode",      (int)d.cull_mode);
	yyjson_mut_obj_add_int (doc, obj, "fill_mode",      (int)d.fill_mode);
	yyjson_mut_obj_add_int (doc, obj, "primitive_type", (int)d.primitive_type);
	yyjson_mut_obj_add_bool(doc, obj, "depth_test",     d.depth_test);
	yyjson_mut_obj_add_bool(doc, obj, "depth_write",    d.depth_write);
	yyjson_mut_obj_add_int (doc, obj, "depth_compare",  (int)d.depth_compare_op);
	yyjson_mut_obj_add_bool(doc, obj, "stencil_test",   d.stencil_test);
	yyjson_mut_obj_add_bool(doc, obj, "color_blend",    d.color_blend);
	yyjson_mut_obj_add_bool(doc, obj, "bias_enable",    d.rasterizer_bias.enable_depth_bias);
	yyjson_mut_obj_add_real(doc, obj, "bias_constant",  d.rasterizer_bias.depth_bias_constant_factor);
	yyjson_mut_obj_add_real(doc, obj, "bias_slope",     d.rasterizer_bias.depth_bias_slope_factor);
	yyjson_mut_obj_add_real(doc, obj, "bias_clamp",     d.rasterizer_bias.depth_bias_clamp);
}
// ── params материала ↔ json ПО СХЕМЕ ТИПА (MaterialParamsSpec) ──
// Тип называет себя строкой (params_type = имя в реестре), поля пишутся ПО ИМЕНАМ: скаляр —
// числом/булем, вектор/цвет — массивом лейнов. Раскладку блоба знает схема, а не этот файл,
// поэтому тип, зарегистрированный кодом игры, сериализуется здесь без единой правки движка.
// Плюс к читаемости — устойчивость: поле, добавленное/удалённое/переставленное в структуре,
// не ломает старые сцены (на загрузке недостающее остаётся дефолтом, лишнее игнорируется).
static bool JsonNum(yyjson_val* v, double& out) {
	if (yyjson_is_real(v)) { out = yyjson_get_real(v); return true; }
	if (yyjson_is_sint(v)) { out = (double)yyjson_get_sint(v); return true; }
	if (yyjson_is_uint(v)) { out = (double)yyjson_get_uint(v); return true; }
	if (yyjson_is_bool(v)) { out = yyjson_get_bool(v) ? 1.0 : 0.0; return true; }
	return false;
}
static void WriteMaterialParams(yyjson_mut_doc* doc, yyjson_mut_val* obj,
                                const MaterialParamsSpec& s, const std::vector<uint8_t>& blob)
{
	for (const MatFieldSpec& f : s.fields) {
		const void* p = MatFieldPtr(blob, f);
		if (!p) continue;                       // блоб короче схемы — поле пропускаем
		const uint32_t lanes = MatFieldLanes(f.kind);
		if (lanes > 1) {
			yyjson_mut_val* arr = yyjson_mut_obj_add_arr(doc, obj, f.key);
			const float* v = static_cast<const float*>(p);
			for (uint32_t i = 0; i < lanes; ++i) yyjson_mut_arr_add_real(doc, arr, v[i]);
		}
		else if (f.kind == MatFieldKind::Bool)
			yyjson_mut_obj_add_bool(doc, obj, f.key, *static_cast<const uint32_t*>(p) != 0);
		else if (f.kind == MatFieldKind::U32)
			yyjson_mut_obj_add_uint(doc, obj, f.key, *static_cast<const uint32_t*>(p));
		else
			yyjson_mut_obj_add_real(doc, obj, f.key, *static_cast<const float*>(p));
	}
}
// Блоб приходит уже заполненным дефолтами типа (s.defaults) — здесь только перекрываем найденное.
static void ReadMaterialParams(yyjson_val* obj, const MaterialParamsSpec& s, std::vector<uint8_t>& blob)
{
	if (!obj) return;
	for (const MatFieldSpec& f : s.fields) {
		yyjson_val* v = yyjson_obj_get(obj, f.key);
		if (!v) continue;                       // ключа нет → остаётся дефолт
		void* p = MatFieldPtr(blob, f);
		if (!p) continue;
		auto put = [&](uint32_t lane, double d) {
			if (f.clamp_on_load && f.lo < f.hi) d = d < f.lo ? f.lo : (d > f.hi ? f.hi : d);
			if (f.kind == MatFieldKind::Bool || f.kind == MatFieldKind::U32)
				static_cast<uint32_t*>(p)[lane] = (uint32_t)(d < 0 ? 0 : d);
			else
				static_cast<float*>(p)[lane] = (float)d;
		};
		const uint32_t lanes = MatFieldLanes(f.kind);
		if (lanes > 1) {
			size_t i, max; yyjson_val* e;
			yyjson_arr_foreach(v, i, max, e) {
				if (i >= lanes) break;          // длиннее схемы → усечь
				double d; if (JsonNum(e, d)) put((uint32_t)i, d);
			}
		}
		else { double d; if (JsonNum(v, d)) put(0, d); }
	}
}

// Имя буфера из файла (std::string) → КАНОНИЧНЫЙ BufferDataName (ключ реестра = const char*
// статического литерала). Совпадение по содержимому (карта ключуется по указателю). nullptr — нет.
static BufferDataName ResolveBufferName(BufferManager* bm, const std::string& s) {
	for (auto& [key, b] : bm->GetBuffersData())
		if (b && b->debug_name == s) return key;
	return nullptr;
}

static ShaderProgramDescription ReadSpd(yyjson_val* obj) {
	ShaderProgramDescription d{};
	if (!obj) return d;
	d.cull_mode      = (SDL_GPUCullMode)     JsonInt(obj, "cull_mode",      (int)d.cull_mode);
	d.fill_mode      = (SDL_GPUFillMode)     JsonInt(obj, "fill_mode",      (int)d.fill_mode);
	d.primitive_type = (SDL_GPUPrimitiveType)JsonInt(obj, "primitive_type", (int)d.primitive_type);
	d.depth_test   = JsonBool(obj, "depth_test",   d.depth_test);
	d.depth_write  = JsonBool(obj, "depth_write",  d.depth_write);
	d.depth_compare_op = (SDL_GPUCompareOp)JsonInt(obj, "depth_compare", (int)d.depth_compare_op);
	d.stencil_test = JsonBool(obj, "stencil_test", d.stencil_test);
	d.color_blend  = JsonBool(obj, "color_blend",  d.color_blend);
	d.rasterizer_bias.enable_depth_bias        = JsonBool(obj, "bias_enable",   false);
	d.rasterizer_bias.depth_bias_constant_factor = (float)JsonReal(obj, "bias_constant", 0.0);
	d.rasterizer_bias.depth_bias_slope_factor    = (float)JsonReal(obj, "bias_slope",    0.0);
	d.rasterizer_bias.depth_bias_clamp           = (float)JsonReal(obj, "bias_clamp",    0.0);
	return d;
}

void Engine::SaveScene(const SceneName& scene_name, const std::string& dir)
{
	SceneData* scene = object_manager->GetScene(scene_name);
	if (!scene) { SDL_Log("SaveScene: scene '%s' not found", scene_name.c_str()); return; }

	std::error_code ec;
	std::filesystem::create_directories(dir, ec);   // папка сцены (уже существует — не ошибка)
	if (ec) { SDL_Log("SaveScene: cannot create dir '%s' (%s)", dir.c_str(), ec.message().c_str()); return; }

	// ── ECS → scene.json (колоночный архетип-формат, см. ObjectManager::SaveScene) ──
	{
		const std::string text = object_manager->SaveScene(scene);
		const std::string path = dir + "/scene.json";
		std::ofstream f(path, std::ios::binary);
		if (!f) { SDL_Log("SaveScene: cannot open '%s' for write", path.c_str()); return; }
		f << text;
	}

	// ── Текстуры → textures.json (merge-манифест). Скип: dont_save (движковые дефолты) и
	//    байтовые (пустой source_path — из файла не пересоздаются, их делает код). ──
	{
		yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
		yyjson_mut_val* root = yyjson_mut_obj(doc);
		yyjson_mut_doc_set_root(doc, root);
		yyjson_mut_val* arr = yyjson_mut_obj_add_arr(doc, root, "textures");
		size_t saved = 0;
		for (auto& [name, h] : texture_manager->GetTextureHandles()) {
			if (!h || h->dont_save || h->source_path.empty()) continue;
			yyjson_mut_val* t = yyjson_mut_arr_add_obj(doc, arr);
			// Кубмапа самоописана f0-гранью: запись одна на куб, имя — логическое (cube_name),
			// флаг "cube" ведёт загрузку через CreateCubeMapTexture (см. LoadScene ниже).
			const bool cube = !h->cube_name.empty();
			yyjson_mut_obj_add_strcpy(doc, t, "name",  cube ? h->cube_name.c_str() : name.c_str());
			yyjson_mut_obj_add_strcpy(doc, t, "atlas", h->atlas_name.c_str());
			yyjson_mut_obj_add_strcpy(doc, t, "path",  h->source_path.c_str());
			yyjson_mut_obj_add_str   (doc, t, "conv",  ConvToStr(h->conv));   // статический литерал — без копии
			if (cube) yyjson_mut_obj_add_bool(doc, t, "cube", true);
			++saved;
		}
		yyjson_write_err werr;
		const std::string path = dir + "/textures.json";
		if (!yyjson_mut_write_file(path.c_str(), doc, YYJSON_WRITE_PRETTY, nullptr, &werr))
			SDL_Log("SaveScene: cannot write '%s' (%s)", path.c_str(), werr.msg);
		else
			SDL_Log("SaveScene: %zu textures -> textures.json", saved);
		yyjson_mut_doc_free(doc);
	}

	// ── Модели → models.json. Скип: dont_save и процедурные (пустой model_path — из файла не
	//    пересоздаются). anchor — числом (стабильный enum, редко инспектируется). ──
	{
		yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
		yyjson_mut_val* root = yyjson_mut_obj(doc);
		yyjson_mut_doc_set_root(doc, root);
		yyjson_mut_val* arr = yyjson_mut_obj_add_arr(doc, root, "models");
		size_t saved = 0;
		for (auto& [name, m] : model_manager->GetModels()) {
			if (!m || m->dont_save || m->model_path.empty()) continue;
			yyjson_mut_val* e = yyjson_mut_arr_add_obj(doc, arr);
			yyjson_mut_obj_add_strcpy(doc, e, "name",   name.c_str());
			yyjson_mut_obj_add_strcpy(doc, e, "vertex", m->model_path.c_str());
			yyjson_mut_obj_add_strcpy(doc, e, "index",  m->index_path.c_str());
			yyjson_mut_obj_add_int   (doc, e, "anchor", (int)m->anchor);
			++saved;
		}
		yyjson_write_err werr;
		const std::string path = dir + "/models.json";
		if (!yyjson_mut_write_file(path.c_str(), doc, YYJSON_WRITE_PRETTY, nullptr, &werr))
			SDL_Log("SaveScene: cannot write '%s' (%s)", path.c_str(), werr.msg);
		else
			SDL_Log("SaveScene: %zu models -> models.json", saved);
		yyjson_mut_doc_free(doc);
	}

	// ── Шейдеры → shaders.json: SD (vertex/fragment/compute) + программы. Скип dont_save и
	//    пустых путей у SD. SP различаются полем "kind": пока сериализуется только "render"
	//    (compute-sp держит указатели, а не имена — см. заготовку в LoadScene). ──
	{
		ShaderManager* sm = shader_manager;
		yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
		yyjson_mut_val* root = yyjson_mut_obj(doc);
		yyjson_mut_doc_set_root(doc, root);

		// -- VSD: имя, путь, раскладка pull (набор+порядок семантик первого биндинга) --
		yyjson_mut_val* vsa = yyjson_mut_obj_add_arr(doc, root, "vertex_shaders");
		for (auto& [name, d] : sm->GetVertexShaders()) {
			if (d.dont_save || d.source_path.empty()) continue;
			yyjson_mut_val* e = yyjson_mut_arr_add_obj(doc, vsa);
			yyjson_mut_obj_add_strcpy(doc, e, "name", name.c_str());
			yyjson_mut_obj_add_strcpy(doc, e, "path", d.source_path.c_str());
			yyjson_mut_val* pull = yyjson_mut_obj_add_arr(doc, e, "pull");
			// Все биндинги, не только первый: со стримами пула их несколько (Pos/UV/NormTan —
			// по слоту на стрим), а pull манифеста — объединение семантик по всем слотам.
			for (const auto& b : d.bindings)
				for (VertexSemantic s : b.pull)
					yyjson_mut_arr_add_str(doc, pull, SemToStr(s));
		}
		// -- FSD / CSD: имя + путь --
		yyjson_mut_val* fsa = yyjson_mut_obj_add_arr(doc, root, "fragment_shaders");
		for (auto& [name, d] : sm->GetFragmentShaders()) {
			if (d.dont_save || d.source_path.empty()) continue;
			yyjson_mut_val* e = yyjson_mut_arr_add_obj(doc, fsa);
			yyjson_mut_obj_add_strcpy(doc, e, "name", name.c_str());
			yyjson_mut_obj_add_strcpy(doc, e, "path", d.source_path.c_str());
		}
		yyjson_mut_val* csa = yyjson_mut_obj_add_arr(doc, root, "compute_shaders");
		for (auto& [name, d] : sm->GetComputeShaders()) {
			if (d.dont_save || d.source_path.empty()) continue;
			yyjson_mut_val* e = yyjson_mut_arr_add_obj(doc, csa);
			yyjson_mut_obj_add_strcpy(doc, e, "name", name.c_str());
			yyjson_mut_obj_add_strcpy(doc, e, "path", d.source_path.c_str());
		}
		// -- SP: сгруппированы ПО ТИПУ (как SD): render_shader_programs / compute_shader_programs,
		//    без поля "kind" внутри записи. Compute — заготовка (пустой массив): compute-sp держит
		//    указатели на буферы/атласы, а не имена → сериализовать пока нечего. --
		yyjson_mut_val* spa = yyjson_mut_obj_add_arr(doc, root, "render_shader_programs");
		for (auto& [name, sp] : sm->GetShaderPrograms()) {
			if (!sp || sp->dont_save) continue;
			yyjson_mut_val* e = yyjson_mut_arr_add_obj(doc, spa);
			yyjson_mut_obj_add_strcpy(doc, e, "name", name.c_str());
			yyjson_mut_obj_add_strcpy(doc, e, "vs",   sp->vs_name.c_str());
			yyjson_mut_obj_add_strcpy(doc, e, "fs",   sp->fs_name.c_str());
			yyjson_mut_obj_add_strcpy(doc, e, "pass",
				sp->associated_render_pass ? sp->associated_render_pass->debug_name.c_str() : "");
			yyjson_mut_val* vbuf = yyjson_mut_obj_add_arr(doc, e, "vs_buffers");
			for (BufferDataName b : sp->vertex_shader_buffer_names) yyjson_mut_arr_add_strcpy(doc, vbuf, b);
			yyjson_mut_val* fbuf = yyjson_mut_obj_add_arr(doc, e, "fs_buffers");
			for (BufferDataName b : sp->fragment_shader_buffer_names) yyjson_mut_arr_add_strcpy(doc, fbuf, b);
			yyjson_mut_val* slots = yyjson_mut_obj_add_arr(doc, e, "slots");
			for (TextureSlotRole r : sp->required_slots) yyjson_mut_arr_add_str(doc, slots, RoleToStr(r));
			yyjson_mut_val* spd = yyjson_mut_obj_add_obj(doc, e, "spd");
			WriteSpd(doc, spd, sp->spd);
		}
		yyjson_mut_obj_add_arr(doc, root, "compute_shader_programs");   // заготовка (см. выше)

		yyjson_write_err werr;
		const std::string path = dir + "/shaders.json";
		if (!yyjson_mut_write_file(path.c_str(), doc, YYJSON_WRITE_PRETTY, nullptr, &werr))
			SDL_Log("SaveScene: cannot write '%s' (%s)", path.c_str(), werr.msg);
		else
			SDL_Log("SaveScene: shaders -> shaders.json");
		yyjson_mut_doc_free(doc);
	}

	// ── Материалы → materials.json. Скип dont_save (кодовая инфраструктура). Текстуры — по роли,
	//    sp — по имени, params — объект именованных полей по схеме типа (params_type). ──
	{
		yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
		yyjson_mut_val* root = yyjson_mut_obj(doc);
		yyjson_mut_doc_set_root(doc, root);
		yyjson_mut_val* arr = yyjson_mut_obj_add_arr(doc, root, "materials");
		size_t saved = 0;
		for (auto& [name, m] : material_manager->GetMaterials()) {
			if (!m || m->dont_save) continue;
			yyjson_mut_val* e = yyjson_mut_arr_add_obj(doc, arr);
			yyjson_mut_obj_add_strcpy(doc, e, "name", name.c_str());
			yyjson_mut_val* sh = yyjson_mut_obj_add_arr(doc, e, "shaders");
			for (auto& s : m->shader_programs) yyjson_mut_arr_add_strcpy(doc, sh, s.c_str());
			yyjson_mut_val* tex = yyjson_mut_obj_add_arr(doc, e, "textures");
			for (auto& [role, tn] : m->textures) {
				yyjson_mut_val* t = yyjson_mut_arr_add_obj(doc, tex);
				yyjson_mut_obj_add_str   (doc, t, "role",    RoleToStr(role));
				yyjson_mut_obj_add_strcpy(doc, t, "texture", tn.c_str());
			}
			// Тип params назван строкой из реестра; поля пишет его схема. Незарегистрированный
			// тип сохранить нечем (раскладка неизвестна) — громко говорим об этом, а не пишем
			// молча битую запись: материал загрузится без params.
			if (const MaterialParamsSpec* ps = MaterialParamsSpecRegistry::Get().ByName(m->params_type)) {
				yyjson_mut_obj_add_strcpy(doc, e, "params_type", m->params_type.c_str());
				WriteMaterialParams(doc, yyjson_mut_obj_add_obj(doc, e, "params"), *ps, m->params);
			}
			else if (!m->params_type.empty() || !m->params.empty())
				SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
					"SaveScene: material '%s' has params of unregistered type '%s' (%zu bytes) - NOT saved",
					name.c_str(), m->params_type.c_str(), m->params.size());
			++saved;
		}
		yyjson_write_err werr;
		const std::string path = dir + "/materials.json";
		if (!yyjson_mut_write_file(path.c_str(), doc, YYJSON_WRITE_PRETTY, nullptr, &werr))
			SDL_Log("SaveScene: cannot write '%s' (%s)", path.c_str(), werr.msg);
		else
			SDL_Log("SaveScene: %zu materials -> materials.json", saved);
		yyjson_mut_doc_free(doc);
	}

	SDL_Log("SaveScene: wrote scene '%s' to '%s'", scene_name.c_str(), dir.c_str());
}

void Engine::LoadScene(const SceneName& scene_name, const std::string& dir)
{
	// Рендер-поток встаёт на всю загрузку (см. Engine::scene_swap_mutex). На всю, а не только
	// на ECS-своп: фазы манифестов перезаливают словари TextureManager/ModelManager/
	// MaterialManager/ShaderManager, а панели редактора их перечисляют с рендер-потока.
	std::lock_guard<std::mutex> scene_guard(scene_swap_mutex);

	// ── Тайминг фаз загрузки (диагностика 5-секундной загрузки 100k). Load — событие
	// разовое, поэтому не через кадровый Prof, а прямым SDL_Log сразу после. ──
	const auto t_read = Prof::Clock::now();
	const std::string scene_path = dir + "/scene.json";
	std::ifstream f(scene_path, std::ios::binary | std::ios::ate);   // ate: сразу в конец — узнать размер
	if (!f) { SDL_Log("LoadScene: cannot open '%s'", scene_path.c_str()); return; }
	// Читаем ФАЙЛ ОДНОЙ аллокацией прямо в строку (без stringstream → без тройной копии
	// 43 МБ: rdbuf-буфер + ss.str()). Одна аллокация точного размера + один read.
	const std::streamoff sz = f.tellg();
	std::string text;
	if (sz > 0) {
		text.resize(static_cast<size_t>(sz));
		f.seekg(0);
		f.read(text.data(), sz);
		text.resize(static_cast<size_t>(f.gcount()));          // усечь до реально прочитанного
	}
	const double read_ms = Prof::MsSince(t_read);

	// ── Ресурсы ПЕРЕД ECS (merge-upsert; фикс-ап указателей сущностей резолвит по словарям).
	//    Текстуры: json-манифест → разобранный список → tm (словарная семантика — его, декод
	//    файла — колбэком через ctx). Отсутствие файла — валидная частичная папка, только лог.
	//    Дальше по этапам: модели/материалы/шейдеры. ──
	double tex_ms = 0.0;
	{
		const auto t_tex = Prof::Clock::now();
		const std::string tex_path = dir + "/textures.json";
		yyjson_read_err rerr;
		if (yyjson_doc* doc = yyjson_read_file(tex_path.c_str(), 0, nullptr, &rerr)) {
			std::vector<SceneTextureEntry> entries;
			yyjson_val* arr = yyjson_obj_get(yyjson_doc_get_root(doc), "textures");
			entries.reserve(yyjson_arr_size(arr));
			size_t idx, max; yyjson_val* t;
			yyjson_arr_foreach(arr, idx, max, t) {
				entries.push_back({ JsonStr(t, "name"), JsonStr(t, "atlas"), JsonStr(t, "path"),
				                    ConvFromStr(JsonStr(t, "conv")), JsonBool(t, "cube", false) });
			}
			yyjson_doc_free(doc);

			const size_t created = texture_manager->LoadSceneTextures(entries,
				[this](const SceneTextureEntry& e) {
					// Кубмапа-крест грузится своим путём: нарезка на 6 граней + слои cube-атласа.
					if (e.cube) return engine_context->CreateCubeMapTexture(e.name, e.atlas, e.path.c_str());
					return engine_context->CreateTextureFromFile(e.name, e.atlas, e.path.c_str(), e.conv);
				});
			SDL_Log("LoadScene: %zu/%zu textures from manifest", created, entries.size());
		}
		else SDL_Log("LoadScene: no textures.json ('%s') - skipped", rerr.msg);
		tex_ms = Prof::MsSince(t_tex);
	}

	// ── Модели: json-манифест → список → mm (сам читает .bin, merge-upsert). ──
	double mdl_ms = 0.0;
	{
		const auto t_mdl = Prof::Clock::now();
		const std::string mpath = dir + "/models.json";
		yyjson_read_err rerr;
		if (yyjson_doc* doc = yyjson_read_file(mpath.c_str(), 0, nullptr, &rerr)) {
			std::vector<SceneModelEntry> entries;
			yyjson_val* arr = yyjson_obj_get(yyjson_doc_get_root(doc), "models");
			entries.reserve(yyjson_arr_size(arr));
			size_t idx, max; yyjson_val* m;
			yyjson_arr_foreach(arr, idx, max, m) {
				entries.push_back({ JsonStr(m, "name"), JsonStr(m, "vertex"), JsonStr(m, "index"),
				                    (AnchorShift)JsonInt(m, "anchor", 0) });
			}
			yyjson_doc_free(doc);
			const size_t loaded_n = model_manager->LoadSceneModels(entries);
			SDL_Log("LoadScene: %zu/%zu models from manifest", loaded_n, entries.size());
		}
		else SDL_Log("LoadScene: no models.json ('%s') - skipped", rerr.msg);
		mdl_ms = Prof::MsSince(t_mdl);
	}

	// ── Шейдеры: SD (vertex/fragment/compute) ДО программ (sp ссылается на них по имени).
	//    Create* перезаписывают запись реестра по имени (merge-upsert сам собой). SP kind="render"
	//    — delete+create; иные kind (compute) — заготовка, пока пропускаем с логом. ──
	double shd_ms = 0.0;
	{
		const auto t_shd = Prof::Clock::now();
		ShaderManager* sm = shader_manager;
		const std::string spath = dir + "/shaders.json";
		yyjson_read_err rerr;
		if (yyjson_doc* doc = yyjson_read_file(spath.c_str(), 0, nullptr, &rerr)) {
			yyjson_val* root = yyjson_doc_get_root(doc);
			size_t idx, max; yyjson_val* e;

			// Инвалидация пайплайнов sp, ссылающихся на перезагруженный SD (как в Upsert*Shader):
			// иначе sp удержал бы пайплайн, собранный из старых данных шейдера.
			auto invalidate_vs = [&](const std::string& n) {
				for (auto& [sn, sp] : sm->GetShaderPrograms())
					if (sp->vs_name == n) pipe_manager->InvalidatePipeline(sp.get(), batch_builder->RebuildEpoch());
			};
			auto invalidate_fs = [&](const std::string& n) {
				for (auto& [sn, sp] : sm->GetShaderPrograms())
					if (sp->fs_name == n) pipe_manager->InvalidatePipeline(sp.get(), batch_builder->RebuildEpoch());
			};

			if (yyjson_val* arr = yyjson_obj_get(root, "vertex_shaders")) {
				yyjson_arr_foreach(arr, idx, max, e) {
					const std::string name = JsonStr(e, "name"), path = JsonStr(e, "path");
					if (name.empty() || path.empty()) continue;
					std::vector<VertexSemantic> pull;
					if (yyjson_val* pa = yyjson_obj_get(e, "pull")) {
						size_t pi, pm; yyjson_val* ps;
						yyjson_arr_foreach(pa, pi, pm, ps) pull.push_back(SemFromStr(yyjson_get_str(ps)));
					}
					if (pull.empty()) {
					}
					// Манифест говорит СЕМАНТИКАМИ (pull) — язык стабилен, файлы сцен не мигрируются.
					// Семантики → имена стрим-буферов пула (POSITION→Pos, UV→UV, NORMAL/TANGENT→NormTan):
					// shadow_vs [POSITION] получает один Pos-стрим — 12 байт/вершину вместо интерлива.
					std::vector<const char*> stream_names = PosUVNormPool::StreamsForSemantics(pull);
					std::vector<std::string> buf_names(stream_names.begin(), stream_names.end());
					sm->CreateVertexShader(name, path.c_str(), buf_names, buffer_manager);
					invalidate_vs(name);
				}
			}
			if (yyjson_val* arr = yyjson_obj_get(root, "fragment_shaders")) {
				yyjson_arr_foreach(arr, idx, max, e) {
					const std::string name = JsonStr(e, "name"), path = JsonStr(e, "path");
					if (name.empty() || path.empty()) continue;
					sm->CreateFragmentShader(name, path.c_str());
					invalidate_fs(name);
				}
			}
			if (yyjson_val* arr = yyjson_obj_get(root, "compute_shaders")) {
				yyjson_arr_foreach(arr, idx, max, e) {
					const std::string name = JsonStr(e, "name"), path = JsonStr(e, "path");
					if (name.empty() || path.empty()) continue;
					sm->CreateComputeShader(name, path.c_str());
					// compute-пайплайны/батчи пересоберём флагами ниже (программы держат имя cs)
				}
			}
			// -- SP: render_shader_programs (тип задаёт массив, а не поле внутри — как SD) --
			if (yyjson_val* arr = yyjson_obj_get(root, "render_shader_programs")) {
				yyjson_arr_foreach(arr, idx, max, e) {
					const std::string name = JsonStr(e, "name");
					if (name.empty()) continue;
					RenderPassStep* pass = pass_manager->GetRenderPassStep(JsonStr(e, "pass"));
					const std::string vs = JsonStr(e, "vs"), fs = JsonStr(e, "fs");

					std::vector<BufferDataName> vbufs, fbufs;
					if (yyjson_val* a = yyjson_obj_get(e, "vs_buffers")) {
						size_t bi, bm2; yyjson_val* bv;
						yyjson_arr_foreach(a, bi, bm2, bv)
							if (const char* s = yyjson_get_str(bv))            // guard: null → не std::string(nullptr)
								if (BufferDataName n = ResolveBufferName(buffer_manager, s)) vbufs.push_back(n);
					}
					if (yyjson_val* a = yyjson_obj_get(e, "fs_buffers")) {
						size_t bi, bm2; yyjson_val* bv;
						yyjson_arr_foreach(a, bi, bm2, bv)
							if (const char* s = yyjson_get_str(bv))
								if (BufferDataName n = ResolveBufferName(buffer_manager, s)) fbufs.push_back(n);
					}
					std::vector<TextureSlotRole> slots;
					if (yyjson_val* a = yyjson_obj_get(e, "slots")) {
						size_t si, sm2; yyjson_val* sv; TextureSlotRole role;
						yyjson_arr_foreach(a, si, sm2, sv)
							if (RoleFromStr(yyjson_get_str(sv), role)) slots.push_back(role);
					}
					const ShaderProgramDescription spd = ReadSpd(yyjson_obj_get(e, "spd"));

					// Merge-upsert: занятое имя = delete+create (кэш пайплайна по sp* — снять ДО).
					// push_func НЕ переносим: код-байндинги пере-привязывает bind_shader_functions
					// в конце LoadScene (перенос по имени ломался бы на переименовании sp).
					auto& progs = sm->GetShaderPrograms();
					if (auto it = progs.find(name); it != progs.end()) {
						pipe_manager->InvalidatePipeline(it->second.get(), batch_builder->RebuildEpoch());
						sm->DeleteShaderProgram(name);
					}
					sm->CreateShaderProgram(name, spd, pass, vs, vbufs, fs, fbufs, slots, buffer_manager);
				}
			}
			// -- compute_shader_programs: заготовка. compute-sp держит указатели (буферы/атласы),
			//    а не имена → пока не грузим, только сообщаем, что запись пропущена. --
			if (yyjson_val* arr = yyjson_obj_get(root, "compute_shader_programs")) {
				yyjson_arr_foreach(arr, idx, max, e)
					SDL_Log("LoadScene: compute shader program '%s' not supported yet - skipped", JsonStr(e, "name"));
			}
			yyjson_doc_free(doc);

			sm->SetDirtyGraphicsPipelines(true);
			sm->SetDirtyComputePipelines(true);
			sm->SetDirtyComputeBatches(true);
			SDL_Log("LoadScene: shaders from manifest");
		}
		else SDL_Log("LoadScene: no shaders.json ('%s') - skipped", rerr.msg);
		shd_ms = Prof::MsSince(t_shd);
	}

	// ── Материалы: json → список → mm (merge-upsert в месте). ПОСЛЕ шейдеров/текстур (ссылается
	//    на них по имени, хотя резолв всё равно ленивый на сборке батча). ──
	double mat_ms = 0.0;
	{
		const auto t_mat = Prof::Clock::now();
		const std::string mpath = dir + "/materials.json";
		yyjson_read_err rerr;
		if (yyjson_doc* doc = yyjson_read_file(mpath.c_str(), 0, nullptr, &rerr)) {
			std::vector<SceneMaterialEntry> entries;
			yyjson_val* arr = yyjson_obj_get(yyjson_doc_get_root(doc), "materials");
			entries.reserve(yyjson_arr_size(arr));
			size_t idx, max; yyjson_val* e;
			yyjson_arr_foreach(arr, idx, max, e) {
				SceneMaterialEntry me;
				me.name = JsonStr(e, "name");
				if (yyjson_val* sh = yyjson_obj_get(e, "shaders")) {
					size_t i, m2; yyjson_val* s;
					yyjson_arr_foreach(sh, i, m2, s) if (const char* n = yyjson_get_str(s)) me.shaders.push_back(n);
				}
				if (yyjson_val* tex = yyjson_obj_get(e, "textures")) {
					size_t i, m2; yyjson_val* t; TextureSlotRole role;
					yyjson_arr_foreach(tex, i, m2, t)
						if (RoleFromStr(JsonStr(t, "role"), role))
							me.textures.emplace_back(role, TextureName(JsonStr(t, "texture")));
				}
				// params: стартуем с ДЕФОЛТОВ типа (member-инициализаторы структуры) и накатываем
				// поля из файла по именам. Тип не зарегистрирован → params нет: раскладки нет,
				// а гадать про байты нельзя (сообщаем, чтобы это не выглядело как «поля потерялись»).
				me.params_type = JsonStr(e, "params_type");
				if (const MaterialParamsSpec* ps = MaterialParamsSpecRegistry::Get().ByName(me.params_type)) {
					me.params = ps->defaults;
					ReadMaterialParams(yyjson_obj_get(e, "params"), *ps, me.params);
				}
				else if (!me.params_type.empty()) {
					SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
						"LoadScene: material '%s' wants params type '%s' - not registered "
						"(register it before LoadScene); loaded without params",
						me.name.c_str(), me.params_type.c_str());
					me.params_type.clear();
				}
				entries.push_back(std::move(me));
			}
			yyjson_doc_free(doc);
			const size_t n = material_manager->LoadSceneMaterials(entries);
			// Сбор usage-флагов: текстуры сцены загружены РАНЬШЕ материалов (фаза tex → mat), поэтому
			// имена уже резолвятся в атласы, и те получают SAMPLER до ближайшего бейка.
			for (const SceneMaterialEntry& e : entries)
				material_manager->CollectSamplerUsage(material_manager->GetMaterial(e.name), texture_manager, e.name);
			SDL_Log("LoadScene: %zu/%zu materials from manifest", n, entries.size());
		}
		else SDL_Log("LoadScene: no materials.json ('%s') - skipped", rerr.msg);
		mat_ms = Prof::MsSince(t_mat);
	}

	// Replace-on-load: сносим прежнее содержимое сцены ДО наполнения — иначе загрузка
	// дописала бы поверх (дубликаты сущностей). Делаем это только после успешного
	// открытия файла, чтобы кривой путь не обнулял текущую сцену.
	//
	// Замок на ECS-swap не нужен: рендер-проходы/каллинг читают пер-слотовые слепки, а не
	// ECS. Единственный живой читатель ECS на рендер-потоке — UI (осознанный компромисс,
	// см. Engine::RenderFunc; правильное закрытие — UI-слепок или построение UI в sim).
	double clear_ms = 0.0, ecs_ms = 0.0, ptr_ms = 0.0;
	std::vector<Entity> loaded;
	{
		const auto t_clear = Prof::Clock::now();
		if (SceneData* prev = object_manager->GetScene(scene_name)) prev->clear();
		clear_ms = Prof::MsSince(t_clear);

		// ECS-часть: текст → сущности (указатели на ассеты пока пустые, только имена).
		// Возвращает ИМЕННО созданные этой загрузкой сущности.
		const auto t_ecs = Prof::Clock::now();
		loaded = object_manager->LoadScene(scene_name, text);
		ecs_ms = Prof::MsSince(t_ecs);

		SceneData* scene = object_manager->GetScene(scene_name);
		if (scene) {
			// Чиним указатели на ассеты по сохранённым именам — это знает только верхний слой.
			// Обходим ТОЛЬКО загруженные сущности (не всю сцену): уже существовавшие в ней
			// производные сущности (напр. дебаг-коллайдеры) держат живые указатели без имён —
			// их трогать нельзя. Незаполнённое/неизвестное имя оставляет nullptr; сборщик батчей
			// такие сущности пропускает (см. BatchBuilder::AddEntityToBatches), но логируем.
			const auto t_ptr = Prof::Clock::now();
			for (Entity e : loaded) {
				if (object_manager->Has<ModelComponent>(scene, e)) {
					ModelComponent& m = object_manager->GetComponent<ModelComponent>(scene, e);
					m.model = m.name.empty() ? nullptr : (*model_manager)[m.name];
					if (!m.model)
						SDL_Log("LoadScene: model '%s' not resolved (entity will not render)", m.name.c_str());
				}
				if (object_manager->Has<MaterialComponent>(scene, e)) {
					MaterialComponent& mc = object_manager->GetComponent<MaterialComponent>(scene, e);
					mc.materials.clear();
					mc.materials.reserve(mc.names.size());
					for (const auto& n : mc.names) {
						Material* mat = material_manager->GetMaterial(n);
						if (!mat) SDL_Log("LoadScene: material '%s' not resolved", n.c_str());
						mc.materials.push_back(mat);
					}
				}
			}
			ptr_ms = Prof::MsSince(t_ptr);

			object_manager->SetSceneState(scene_name, true);

			// UI — часть сцены и уходит вместе с ней: clear выше снёс его энтити, здесь сносим
			// дерево. Раньше дерево переживало загрузку (живёт в UI_Yoga, не в ECS) — на экране
			// UI пропадал, а его узлы продолжали висеть в панели иерархии. Новый UI появится,
			// когда дерево начнёт грузиться из файлов сцены.
			if (ui_yoga) ui_yoga->Reset();

			// Скайбокс движок больше НЕ создаёт: фон — обычная сущность сцены (scene.json,
			// Draw+Material+Model БЕЗ Positions — transformless-дровабл, PIB=-1), а его модель/
			// шейдеры/материал/текстура — ресурсы сцены из её манифестов. У каждой сцены свой.
		}
	}

	// Пере-привязка код-байндингов (push_func) к загруженным sp — колбэк игры (у неё живут
	// лямбды). После КАЖДОЙ загрузки, в т.ч. UI-рантаймовой: sp из манифеста пересозданы голыми.
	if (bind_shader_functions) bind_shader_functions();

	// Бейк GPU-ресурсов здесь НЕ делается: он дренируется каждый кадр в начале Engine::PrepareFunc
	// (BakePending). Всё, что объявила эта загрузка (атласы, буферы, sp, материалы), попадёт в
	// ближайший prepare — игровой апдейт и prepare идут последовательно на одном sim-потоке.
	batch_builder->SetDirtyBatches(true);
	SDL_Log("LoadScene: loaded scene '%s' from '%s'", scene_name.c_str(), dir.c_str());
	SDL_Log("LoadScene TIMING [%zu ent, %.1f MB]: read=%.1f  tex=%.1f  mdl=%.1f  shd=%.1f  mat=%.1f  clear=%.1f  ecs=%.1f  ptr_restore=%.1f  | total=%.1f ms",
		loaded.size(), text.size() / (1024.0 * 1024.0),
		read_ms, tex_ms, mdl_ms, shd_ms, mat_ms, clear_ms, ecs_ms, ptr_ms,
		read_ms + tex_ms + mdl_ms + shd_ms + mat_ms + clear_ms + ecs_ms + ptr_ms);
}
