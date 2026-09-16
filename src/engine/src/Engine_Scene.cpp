#include "PCH.h"
#include "Engine.h"
#include "EngineProfiler.h"
#include "ObjectManager.h"
#include "TextureManager.h"
#include "ModelManager.h"
#include "MaterialManager.h"
#include "ParamsSpec.h"
#include "ShaderManager.h"
#include "ResourceTags.h"
#include "PipeManager.h"
#include "PassManager.h"
#include "BufferManager.h"
#include "PositionStructure.h"
#include "BatchBuilder.h"
#include "EngineContext.h"
#include "UI_Yoga.h"
#include <fstream>
#include <filesystem>
#include "yyjson.h"

using namespace ShaderBase;

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

static void WriteDefines(yyjson_mut_doc* doc, yyjson_mut_val* obj, const std::vector<ShaderDefine>& defs) {
	if (defs.empty()) return;
	yyjson_mut_val* d = yyjson_mut_obj_add_obj(doc, obj, "defines");
	for (const ShaderDefine& def : defs)
		yyjson_mut_obj_add_strcpy(doc, d, def.name.c_str(), def.value.c_str());
}

static std::vector<ShaderDefine> ReadDefines(yyjson_val* obj) {
	std::vector<ShaderDefine> out;
	yyjson_val* d = yyjson_obj_get(obj, "defines");
	if (!d || !yyjson_is_obj(d)) return out;
	yyjson_obj_iter it; yyjson_obj_iter_init(d, &it);
	while (yyjson_val* k = yyjson_obj_iter_next(&it)) {
		const char* kn = yyjson_get_str(k);
		const char* vv = yyjson_get_str(yyjson_obj_iter_get_val(k));
		if (kn) out.push_back({ kn, vv ? vv : "" });
	}
	return out;
}

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
static bool JsonNum(yyjson_val* v, double& out) {
	if (yyjson_is_real(v)) { out = yyjson_get_real(v); return true; }
	if (yyjson_is_sint(v)) { out = (double)yyjson_get_sint(v); return true; }
	if (yyjson_is_uint(v)) { out = (double)yyjson_get_uint(v); return true; }
	if (yyjson_is_bool(v)) { out = yyjson_get_bool(v) ? 1.0 : 0.0; return true; }
	return false;
}
static void WriteMaterialParams(yyjson_mut_doc* doc, yyjson_mut_val* obj,
                                const ParamsSpec& s, const std::vector<uint8_t>& blob)
{
	for (const ParamsFieldSpec& f : s.fields) {
		const void* p = ParamsFieldPtr(blob, f);
		if (!p) continue;
		const uint32_t lanes = ParamsFieldLanes(f.kind);
		if (lanes > 1) {
			yyjson_mut_val* arr = yyjson_mut_obj_add_arr(doc, obj, f.key);
			const float* v = static_cast<const float*>(p);
			for (uint32_t i = 0; i < lanes; ++i) yyjson_mut_arr_add_real(doc, arr, v[i]);
		}
		else if (f.kind == ParamsFieldKind::Bool)
			yyjson_mut_obj_add_bool(doc, obj, f.key, *static_cast<const uint32_t*>(p) != 0);
		else if (f.kind == ParamsFieldKind::U32)
			yyjson_mut_obj_add_uint(doc, obj, f.key, *static_cast<const uint32_t*>(p));
		else
			yyjson_mut_obj_add_real(doc, obj, f.key, *static_cast<const float*>(p));
	}
}
static void ReadMaterialParams(yyjson_val* obj, const ParamsSpec& s, std::vector<uint8_t>& blob)
{
	if (!obj) return;
	for (const ParamsFieldSpec& f : s.fields) {
		yyjson_val* v = yyjson_obj_get(obj, f.key);
		if (!v) continue;
		void* p = ParamsFieldPtr(blob, f);
		if (!p) continue;
		auto put = [&](uint32_t lane, double d) {
			if (f.clamp_on_load && f.lo < f.hi) d = d < f.lo ? f.lo : (d > f.hi ? f.hi : d);
			if (f.kind == ParamsFieldKind::Bool || f.kind == ParamsFieldKind::U32)
				static_cast<uint32_t*>(p)[lane] = (uint32_t)(d < 0 ? 0 : d);
			else
				static_cast<float*>(p)[lane] = (float)d;
		};
		const uint32_t lanes = ParamsFieldLanes(f.kind);
		if (lanes > 1) {
			size_t i, max; yyjson_val* e;
			yyjson_arr_foreach(v, i, max, e) {
				if (i >= lanes) break;
				double d; if (JsonNum(e, d)) put((uint32_t)i, d);
			}
		}
		else { double d; if (JsonNum(v, d)) put(0, d); }
	}
}

static BufferDataName ResolveBufferName(BufferManager* bm, const std::string& s) {
	const BufferDataRegistry& reg = bm->GetBuffersData();
	auto it = reg.find(s.c_str());
	return it != reg.end() ? it->first : nullptr;
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

template<class Fn> static void ForEachVal(yyjson_val* arr, Fn&& fn)
{
	if (!arr) return;
	size_t i, max; yyjson_val* v;
	yyjson_arr_foreach(arr, i, max, v) fn(v);
}
template<class Fn> static void ForEachIn(yyjson_val* obj, const char* key, Fn&& fn)
{
	ForEachVal(yyjson_obj_get(obj, key), std::forward<Fn>(fn));
}

struct MutDoc {
	yyjson_mut_doc* doc  = yyjson_mut_doc_new(nullptr);
	yyjson_mut_val* root = yyjson_mut_obj(doc);

	MutDoc() { yyjson_mut_doc_set_root(doc, root); }
	~MutDoc() { yyjson_mut_doc_free(doc); }
	MutDoc(const MutDoc&) = delete;
	MutDoc& operator=(const MutDoc&) = delete;

	yyjson_mut_val* Arr(const char* key) { return yyjson_mut_obj_add_arr(doc, root, key); }

	void Write(const std::string& dir, const char* file, const char* what, size_t count) const
	{
		const std::string path = dir + "/" + file;
		yyjson_write_err werr;
		if (!yyjson_mut_write_file(path.c_str(), doc, YYJSON_WRITE_PRETTY, nullptr, &werr))
			SDL_Log("SaveScene: cannot write '%s' (%s)", path.c_str(), werr.msg);
		else if (count == kNoCount)
			SDL_Log("SaveScene: %s -> %s", what, file);
		else
			SDL_Log("SaveScene: %zu %s -> %s", count, what, file);
	}
	static constexpr size_t kNoCount = static_cast<size_t>(-1);
};

struct ReadDoc {
	yyjson_doc* doc = nullptr;

	ReadDoc(const std::string& dir, const char* file)
	{
		const std::string path = dir + "/" + file;
		yyjson_read_err rerr;
		doc = yyjson_read_file(path.c_str(), 0, nullptr, &rerr);
		if (!doc) SDL_Log("LoadScene: no %s ('%s') - skipped", file, rerr.msg);
	}
	~ReadDoc() { if (doc) yyjson_doc_free(doc); }
	ReadDoc(const ReadDoc&) = delete;
	ReadDoc& operator=(const ReadDoc&) = delete;

	explicit operator bool() const { return doc != nullptr; }
	yyjson_val* root() const { return yyjson_doc_get_root(doc); }
};

static const char* CStrOf(const std::string& s) { return s.c_str(); }
static const char* CStrOf(const char* s)        { return s; }

template<class Range>
static void WriteStrArray(yyjson_mut_doc* doc, yyjson_mut_val* obj, const char* key, const Range& names)
{
	yyjson_mut_val* arr = yyjson_mut_obj_add_arr(doc, obj, key);
	for (const auto& n : names) yyjson_mut_arr_add_strcpy(doc, arr, CStrOf(n));
}

template<class Range, class NameOf>
static void WriteIdArray(yyjson_mut_doc* doc, yyjson_mut_val* obj, const char* key, const Range& ids, NameOf name_of)
{
	yyjson_mut_val* arr = yyjson_mut_obj_add_arr(doc, obj, key);
	for (const auto& id : ids) yyjson_mut_arr_add_strcpy(doc, arr, name_of(id).c_str());
}

static std::vector<BufferDataName> ReadBufferNames(BufferManager* bm, yyjson_val* obj, const char* key)
{
	std::vector<BufferDataName> out;
	ForEachIn(obj, key, [&](yyjson_val* v) {
		if (const char* s = yyjson_get_str(v))
			if (BufferDataName n = ResolveBufferName(bm, s)) out.push_back(n);
	});
	return out;
}
static std::vector<AtlasName> ReadAtlasNames(yyjson_val* obj, const char* key)
{
	std::vector<AtlasName> out;
	ForEachIn(obj, key, [&](yyjson_val* v) { if (const char* s = yyjson_get_str(v)) out.emplace_back(s); });
	return out;
}
static std::vector<TextureSlotRole> ReadRoles(yyjson_val* obj, const char* key)
{
	std::vector<TextureSlotRole> out;
	ForEachIn(obj, key, [&](yyjson_val* v) {
		TextureSlotRole role;
		if (RoleFromStr(yyjson_get_str(v), role)) out.push_back(role);
	});
	return out;
}

struct PhaseTimer {
	double& out;
	Prof::Clock::time_point start = Prof::Clock::now();
	explicit PhaseTimer(double& dst) : out(dst) {}
	~PhaseTimer() { out = Prof::MsSince(start); }
};

static void SaveTextures(const std::string& dir, TextureManager* tm)
{
	MutDoc d;
	yyjson_mut_val* arr = d.Arr("textures");
	size_t saved = 0;
	for (int32_t i = 0; i < tm->Textures().Count(); ++i) {
		const TextureCell& cell = tm->Textures().At(i);
		const std::string& name = cell.name;
		const std::shared_ptr<TextureHandle>& h = cell.object;
		if (!h || HasTag(h->tags, ResourceTag::CodeOwned) || h->source_path.empty()) continue;
		yyjson_mut_val* t = yyjson_mut_arr_add_obj(d.doc, arr);
		const bool cube = h->atlas && (h->atlas->texture_type == SDL_GPU_TEXTURETYPE_CUBE
		                            || h->atlas->texture_type == SDL_GPU_TEXTURETYPE_CUBE_ARRAY);
		yyjson_mut_obj_add_strcpy(d.doc, t, "name",  name.c_str());
		yyjson_mut_obj_add_strcpy(d.doc, t, "atlas", tm->AtlasNameOf(h->atlas_id).c_str());
		yyjson_mut_obj_add_strcpy(d.doc, t, "path",  h->source_path.c_str());
		yyjson_mut_obj_add_str   (d.doc, t, "conv",  ConvToStr(h->conv));
		if (cube) yyjson_mut_obj_add_bool(d.doc, t, "cube", true);
		++saved;
	}
	d.Write(dir, "textures.json", "textures", saved);
}

static void SaveModels(const std::string& dir, ModelManager* mm)
{
	MutDoc d;
	yyjson_mut_val* arr = d.Arr("models");
	size_t saved = 0;
	for (int32_t mi = 0; mi < mm->Models().Count(); ++mi) {
		const ModelCell& cell = mm->Models().At(mi);
		const ModelData* m = cell.object.get();
		const std::string& name = cell.name;
		if (!m || HasTag(m->tags, ResourceTag::CodeOwned) || m->model_path.empty()) continue;
		yyjson_mut_val* e = yyjson_mut_arr_add_obj(d.doc, arr);
		yyjson_mut_obj_add_strcpy(d.doc, e, "name",   name.c_str());
		yyjson_mut_obj_add_strcpy(d.doc, e, "vertex", m->model_path.c_str());
		yyjson_mut_obj_add_strcpy(d.doc, e, "index",  m->index_path.c_str());
		yyjson_mut_obj_add_int   (d.doc, e, "anchor", (int)m->anchor);
		yyjson_mut_obj_add_strcpy(d.doc, e, "pool",   m->pool_name.c_str());
		yyjson_mut_val* span = yyjson_mut_obj_add_arr(d.doc, e, "screen_size_span");
		for (const SubMeshData& sm : m->submeshes) {
			yyjson_mut_val* pair = yyjson_mut_arr_add_arr(d.doc, span);
			yyjson_mut_arr_add_int(d.doc, pair, sm.screen_size_span.lod_min);
			yyjson_mut_arr_add_int(d.doc, pair, sm.screen_size_span.lod_max);
		}
		++saved;
	}
	d.Write(dir, "models.json", "models", saved);
}

static void SaveShaders(const std::string& dir, ShaderManager* sm, TextureManager* tm)
{
	MutDoc d;

	yyjson_mut_val* vsa = d.Arr("vertex_shaders");
	for (int32_t i = 0; i < sm->VertexShaders().Count(); ++i) {
		const VertexShaderCell& cell = sm->VertexShaders().At(i);
		const VertexShaderData* vsp = cell.object.get();
		if (!vsp) continue;
		const VertexShaderData& vs = *vsp;
		if (HasTag(vs.tags, ResourceTag::CodeOwned) || vs.source_path.empty()) continue;
		yyjson_mut_val* e = yyjson_mut_arr_add_obj(d.doc, vsa);
		yyjson_mut_obj_add_strcpy(d.doc, e, "name", cell.name.c_str());
		yyjson_mut_obj_add_strcpy(d.doc, e, "path", vs.source_path.c_str());
		yyjson_mut_obj_add_strcpy(d.doc, e, "pool", vs.pool_name.c_str());
		yyjson_mut_val* pull = yyjson_mut_obj_add_arr(d.doc, e, "pull");
		for (const auto& b : vs.bindings)
			for (VertexSemantic s : b.pull)
				yyjson_mut_arr_add_str(d.doc, pull, SemToStr(s));
		WriteDefines(d.doc, e, vs.defines);
	}

	auto write_sd = [&](const char* key, const auto& registry) {
		yyjson_mut_val* arr = d.Arr(key);
		for (int32_t i = 0; i < registry.Count(); ++i) {
			const auto& cell = registry.At(i);
			const auto* sd = cell.object.get();
			if (!sd || HasTag(sd->tags, ResourceTag::CodeOwned) || sd->source_path.empty()) continue;
			yyjson_mut_val* e = yyjson_mut_arr_add_obj(d.doc, arr);
			yyjson_mut_obj_add_strcpy(d.doc, e, "name", cell.name.c_str());
			yyjson_mut_obj_add_strcpy(d.doc, e, "path", sd->source_path.c_str());
			WriteDefines(d.doc, e, sd->defines);
		}
	};
	write_sd("fragment_shaders", sm->FragmentShaders());
	write_sd("compute_shaders",  sm->ComputeShaders());

	yyjson_mut_val* spa = d.Arr("render_shader_programs");
	for (int32_t spi = 0; spi < sm->ShaderPrograms().Count(); ++spi) {
		const ShaderProgramCell& spcell = sm->ShaderPrograms().At(spi);
		const ShaderProgram* sp = spcell.object.get();
		const std::string& name = spcell.name;
		if (!sp || HasTag(sp->tags, ResourceTag::CodeOwned)) continue;
		yyjson_mut_val* e = yyjson_mut_arr_add_obj(d.doc, spa);
		yyjson_mut_obj_add_strcpy(d.doc, e, "name", name.c_str());
		yyjson_mut_obj_add_strcpy(d.doc, e, "vs",   sm->VertexShaders().NameOf(sp->vs_id).c_str());
		yyjson_mut_obj_add_strcpy(d.doc, e, "fs",   sm->FragmentShaders().NameOf(sp->fs_id).c_str());
		yyjson_mut_obj_add_strcpy(d.doc, e, "pass", sp->render_pass_name.c_str());
		WriteStrArray(d.doc, e, "vs_buffers", sp->vertex_shader_buffer_names);
		WriteStrArray(d.doc, e, "fs_buffers", sp->fragment_shader_buffer_names);
		yyjson_mut_val* slots = yyjson_mut_obj_add_arr(d.doc, e, "slots");
		for (TextureSlotRole r : sp->required_slots) yyjson_mut_arr_add_str(d.doc, slots, RoleToStr(r));
		WriteSpd(d.doc, yyjson_mut_obj_add_obj(d.doc, e, "spd"), sp->spd);
	}

	yyjson_mut_val* cspa = d.Arr("compute_shader_programs");
	for (int32_t cspi = 0; cspi < sm->ComputePrograms().Count(); ++cspi) {
		const ComputeProgramCell& cspcell = sm->ComputePrograms().At(cspi);
		const ComputeShaderProgram* csp = cspcell.object.get();
		const std::string& csp_name = cspcell.name;
		if (!csp || HasTag(csp->tags, ResourceTag::CodeOwned)) continue;
		yyjson_mut_val* e = yyjson_mut_arr_add_obj(d.doc, cspa);
		yyjson_mut_obj_add_strcpy(d.doc, e, "name", csp_name.c_str());
		yyjson_mut_obj_add_strcpy(d.doc, e, "cs",   sm->ComputeShaders().NameOf(csp->cs_id).c_str());
		yyjson_mut_obj_add_strcpy(d.doc, e, "pass", csp->compute_pass_name.c_str());
		WriteStrArray(d.doc, e, "rw_buffers", csp->rw_storage_buffer_names);
		WriteStrArray(d.doc, e, "ro_buffers", csp->ro_storage_buffer_names);
		yyjson_mut_val* rwt = yyjson_mut_obj_add_arr(d.doc, e, "rw_textures");
		for (const auto& t : csp->rw_storage_textures) {
			yyjson_mut_val* te = yyjson_mut_arr_add_obj(d.doc, rwt);
			yyjson_mut_obj_add_strcpy(d.doc, te, "atlas", tm->AtlasNameOf(t.texture_atlas).c_str());
			yyjson_mut_obj_add_uint(d.doc, te, "mip",   t.mip_level);
			yyjson_mut_obj_add_uint(d.doc, te, "layer", t.layer);
			yyjson_mut_obj_add_bool(d.doc, te, "simultaneous", t.need_simultaneous);
		}
		auto atlas_name = [tm](AtlasId id) -> const std::string& { return tm->AtlasNameOf(id); };
		WriteIdArray(d.doc, e, "ro_textures", csp->ro_storage_texture_ids, atlas_name);
		WriteIdArray(d.doc, e, "samplers",    csp->texture_sampler_ids,    atlas_name);
	}

	d.Write(dir, "shaders.json", "shaders", MutDoc::kNoCount);
}

static void SaveMaterials(const std::string& dir, MaterialManager* mtm, TextureManager* tm, ShaderManager* sm)
{
	MutDoc d;
	yyjson_mut_val* arr = d.Arr("materials");
	size_t saved = 0;
	for (int32_t mi = 0; mi < mtm->Materials().Count(); ++mi) {
		const MaterialCell& cell = mtm->Materials().At(mi);
		const Material* m = cell.object.get();
		const std::string& name = cell.name;
		if (!m || HasTag(m->tags, ResourceTag::CodeOwned)) continue;
		yyjson_mut_val* e = yyjson_mut_arr_add_obj(d.doc, arr);
		yyjson_mut_obj_add_strcpy(d.doc, e, "name", name.c_str());

		yyjson_mut_val* sh = yyjson_mut_obj_add_arr(d.doc, e, "shaders");
		for (const SpBinding& b : m->shader_programs) {
			yyjson_mut_val* so = yyjson_mut_arr_add_obj(d.doc, sh);
			yyjson_mut_obj_add_strcpy(d.doc, so, "name", sm->ShaderProgramNameOf(b.sp).c_str());
			if (!b.params || b.params->empty()) continue;
			if (const ParamsSpec* ps = ParamsSpecRegistry::Materials().ByName(b.params_type)) {
				yyjson_mut_obj_add_strcpy(d.doc, so, "params_type", b.params_type.c_str());
				WriteMaterialParams(d.doc, yyjson_mut_obj_add_obj(d.doc, so, "params"), *ps, *b.params);
			}
			else
				SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
					"SaveScene: material '%s' sp '%s' has params of unregistered type '%s' (%zu bytes) - NOT saved",
					name.c_str(), sm->ShaderProgramNameOf(b.sp).c_str(), b.params_type.c_str(), b.params->size());
		}

		yyjson_mut_val* tex = yyjson_mut_obj_add_arr(d.doc, e, "textures");
		for (auto& [role, tns] : m->textures) {
			yyjson_mut_val* t = yyjson_mut_arr_add_obj(d.doc, tex);
			yyjson_mut_obj_add_str(d.doc, t, "role", RoleToStr(role));
			WriteIdArray(d.doc, t, "textures", tns, [tm](TextureId id) -> const std::string& { return tm->TextureNameOf(id); });
		}
		++saved;
	}
	d.Write(dir, "materials.json", "materials", saved);
}

void Engine::SaveScene(const SceneName& scene_name, const std::string& scenes_root)
{
	SceneData* scene = object_manager->GetScene(scene_name);
	if (!scene) { SDL_Log("SaveScene: scene '%s' not found", scene_name.c_str()); return; }

	const std::string dir = scenes_root + "/" + scene_name;

	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	if (ec) { SDL_Log("SaveScene: cannot create dir '%s' (%s)", dir.c_str(), ec.message().c_str()); return; }

	{
		const std::string text = object_manager->SaveScene(scene);
		const std::string path = dir + "/scene.json";
		std::ofstream f(path, std::ios::binary);
		if (!f) { SDL_Log("SaveScene: cannot open '%s' for write", path.c_str()); return; }
		f << text;
	}

	SaveTextures (dir, texture_manager);
	SaveModels   (dir, model_manager);
	SaveShaders  (dir, shader_manager, texture_manager);
	SaveMaterials(dir, material_manager, texture_manager, shader_manager);

	SDL_Log("SaveScene: wrote scene '%s' to '%s'", scene_name.c_str(), dir.c_str());
}

static void LoadTextures(const std::string& dir, TextureManager* tm, EngineContext* ctx)
{
	ReadDoc d(dir, "textures.json");
	if (!d) return;

	std::vector<SceneTextureEntry> entries;
	ForEachIn(d.root(), "textures", [&](yyjson_val* t) {
		entries.push_back({ JsonStr(t, "name"), JsonStr(t, "atlas"), JsonStr(t, "path"),
		                    ConvFromStr(JsonStr(t, "conv")), JsonBool(t, "cube", false) });
	});

	const size_t created = tm->LoadSceneTextures(entries, [ctx](const SceneTextureEntry& e) {
		if (e.cube) return ctx->CreateCubeMapTexture(e.name, e.atlas, e.path.c_str());
		return ctx->CreateTextureFromFile(e.name, e.atlas, e.path.c_str(), e.conv);
	});
	SDL_Log("LoadScene: %zu/%zu textures from manifest", created, entries.size());
}

static void LoadModels(const std::string& dir, ModelManager* mm)
{
	ReadDoc d(dir, "models.json");
	if (!d) return;

	std::vector<SceneModelEntry> entries;
	ForEachIn(d.root(), "models", [&](yyjson_val* m) {
		SceneModelEntry entry{ JsonStr(m, "name"), JsonStr(m, "vertex"), JsonStr(m, "index"),
		                       (AnchorShift)JsonInt(m, "anchor", 0), JsonStr(m, "pool") };
		ForEachIn(m, "screen_size_span", [&](yyjson_val* pair) {
			entry.screen_size_span.push_back(
				{ safe_i_u8((int)yyjson_get_int(yyjson_arr_get(pair, 0))),
				  safe_i_u8((int)yyjson_get_int(yyjson_arr_get(pair, 1))) });
		});
		entries.push_back(std::move(entry));
	});

	const size_t loaded = mm->LoadSceneModels(entries);
	SDL_Log("LoadScene: %zu/%zu models from manifest", loaded, entries.size());
}

static void LoadShaderData(yyjson_val* root, ShaderManager* sm, ModelManager* mm, BufferManager* bm)
{
	auto invalidate = [sm](const std::string& name, bool vertex) {
		const VertexShaderId   vs_id = sm->VertexShaders().Find(name);
		const FragmentShaderId fs_id = sm->FragmentShaders().Find(name);
		for (int32_t i = 0; i < sm->ShaderPrograms().Count(); ++i)
			if (ShaderProgram* sp = sm->ShaderPrograms().At(i).object.get())
				if (vertex ? (sp->vs_id == vs_id) : (sp->fs_id == fs_id)) sp->pipeline.reset();
	};

	ForEachIn(root, "vertex_shaders", [&](yyjson_val* e) {
		const std::string name = JsonStr(e, "name"), path = JsonStr(e, "path");
		if (name.empty() || path.empty()) return;
		std::vector<VertexSemantic> pull;
		ForEachIn(e, "pull", [&](yyjson_val* s) { pull.push_back(SemFromStr(yyjson_get_str(s))); });
		sm->CreateVertexShader(name, path.c_str(), mm->GetPool(JsonStr(e, "pool")), pull, bm, ReadDefines(e));
		invalidate(name, /*vertex=*/true);
	});

	ForEachIn(root, "fragment_shaders", [&](yyjson_val* e) {
		const std::string name = JsonStr(e, "name"), path = JsonStr(e, "path");
		if (name.empty() || path.empty()) return;
		sm->CreateFragmentShader(name, path.c_str(), ReadDefines(e));
		invalidate(name, /*vertex=*/false);
	});

	ForEachIn(root, "compute_shaders", [&](yyjson_val* e) {
		const std::string name = JsonStr(e, "name"), path = JsonStr(e, "path");
		if (name.empty() || path.empty()) return;
		sm->CreateComputeShader(name, path.c_str(), ReadDefines(e));
	});
}

static void LoadRenderPrograms(yyjson_val* root, ShaderManager* sm, BufferManager* bm)
{
	ForEachIn(root, "render_shader_programs", [&](yyjson_val* e) {
		const std::string name = JsonStr(e, "name");
		if (name.empty()) return;
		sm->DeleteShaderProgram(sm->ShaderProgramIdOf(name));
		sm->CreateShaderProgram(name, ReadSpd(yyjson_obj_get(e, "spd")), JsonStr(e, "pass"),
			JsonStr(e, "vs"), ReadBufferNames(bm, e, "vs_buffers"),
			JsonStr(e, "fs"), ReadBufferNames(bm, e, "fs_buffers"),
			ReadRoles(e, "slots"), bm);
	});
}

static void LoadComputePrograms(yyjson_val* root, ShaderManager* sm, BufferManager* bm, TextureManager* tm)
{
	size_t made = 0, total = 0;
	ForEachIn(root, "compute_shader_programs", [&](yyjson_val* e) {
		++total;
		const std::string name = JsonStr(e, "name");
		if (name.empty()) return;

		std::vector<ComputeRWTextureBindingParametr> rw_tex;
		ForEachIn(e, "rw_textures", [&](yyjson_val* t) {
			ComputeRWTextureBindingParametr b{};
			b.texture_atlas     = JsonStr(t, "atlas");
			b.mip_level         = safe_i_u32(JsonInt(t, "mip", 0));
			b.layer             = safe_i_u32(JsonInt(t, "layer", 0));
			b.need_simultaneous = JsonBool(t, "simultaneous", false);
			if (!b.texture_atlas.empty()) rw_tex.push_back(std::move(b));
		});

		if (sm->CreateComputeShaderProgram(name, JsonStr(e, "cs"),
				ReadBufferNames(bm, e, "rw_buffers"), ReadBufferNames(bm, e, "ro_buffers"),
				std::move(rw_tex), ReadAtlasNames(e, "ro_textures"), ReadAtlasNames(e, "samplers"),
				JsonStr(e, "pass"), bm, tm))
			++made;
	});
	if (total) SDL_Log("LoadScene: %zu/%zu compute shader programs from manifest", made, total);
}

static void LoadShaders(const std::string& dir, ShaderManager* sm, ModelManager* mm,
                        BufferManager* bm, TextureManager* tm)
{
	ReadDoc d(dir, "shaders.json");
	if (!d) return;

	LoadShaderData     (d.root(), sm, mm, bm);
	LoadRenderPrograms (d.root(), sm, bm);
	LoadComputePrograms(d.root(), sm, bm, tm);

	sm->SetDirtyGraphicsPipelines(true);
	sm->SetDirtyComputePipelines(true);
	sm->SetDirtyComputeBatches(true);
	SDL_Log("LoadScene: shaders from manifest");
}

static void LoadMaterials(const std::string& dir, MaterialManager* mtm, TextureManager* tm, ShaderManager* sm)
{
	ReadDoc d(dir, "materials.json");
	if (!d) return;

	std::vector<SceneMaterialEntry> entries;
	ForEachIn(d.root(), "materials", [&](yyjson_val* e) {
		SceneMaterialEntry me;
		me.name = JsonStr(e, "name");

		ForEachIn(e, "shaders", [&](yyjson_val* s) {
			SceneShaderEntry se;
			se.name = JsonStr(s, "name");
			if (se.name.empty()) return;
			se.params_type = JsonStr(s, "params_type");
			if (const ParamsSpec* ps = ParamsSpecRegistry::Materials().ByName(se.params_type)) {
				se.params = ps->defaults;
				ReadMaterialParams(yyjson_obj_get(s, "params"), *ps, se.params);
			}
			else if (!se.params_type.empty()) {
				SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
					"LoadScene: material '%s' sp '%s' wants params type '%s' - not registered "
					"(register it before LoadScene); loaded without params",
					me.name.c_str(), se.name.c_str(), se.params_type.c_str());
				se.params_type.clear();
			}
			me.shaders.push_back(std::move(se));
		});

		ForEachIn(e, "textures", [&](yyjson_val* t) {
			TextureSlotRole role;
			if (!RoleFromStr(JsonStr(t, "role"), role)) return;
			std::vector<TextureName> names;
			if (yyjson_val* arr = yyjson_obj_get(t, "textures"))
				ForEachVal(arr, [&](yyjson_val* s) { if (const char* str = yyjson_get_str(s)) names.emplace_back(str); });
			else
				names.emplace_back(JsonStr(t, "texture"));
			me.textures.emplace_back(role, std::move(names));
		});

		entries.push_back(std::move(me));
	});

	const size_t n = mtm->LoadSceneMaterials(entries, tm, sm);
	for (const SceneMaterialEntry& e : entries)
		mtm->CollectSamplerUsage(mtm->GetMaterial(e.name), tm, e.name);
	SDL_Log("LoadScene: %zu/%zu materials from manifest", n, entries.size());
}

static void ClearSceneResources(TextureManager* tm, ModelManager* mm, ShaderManager* sm, MaterialManager* mtm)
{
	const size_t mat = mtm->ClearSceneMaterials();
	const size_t shd = sm->ClearSceneShaders();
	const size_t mdl = mm->ClearSceneModels();
	const size_t tex = tm->ClearSceneTextures();
	SDL_Log("LoadScene: wiped %zu materials, %zu shader records, %zu models, %zu textures", mat, shd, mdl, tex);
}

void Engine::LoadScene(const SceneName& scene_name, const std::string& scenes_root)
{
	std::lock_guard<std::mutex> scene_guard(scene_swap_mutex);

	const std::string dir = scenes_root + "/" + scene_name;

	double read_ms = 0, wipe_ms = 0, tex_ms = 0, mdl_ms = 0, shd_ms = 0, mat_ms = 0, clear_ms = 0, ecs_ms = 0;

	std::string text;
	{
		PhaseTimer t(read_ms);
		const std::string scene_path = dir + "/scene.json";
		std::ifstream f(scene_path, std::ios::binary | std::ios::ate);
		if (!f) { SDL_Log("LoadScene: cannot open '%s'", scene_path.c_str()); return; }
		const std::streamoff sz = f.tellg();
		if (sz > 0) {
			text.resize(static_cast<size_t>(sz));
			f.seekg(0);
			f.read(text.data(), sz);
			text.resize(static_cast<size_t>(f.gcount()));
		}
	}

	{ PhaseTimer t(wipe_ms); ClearSceneResources(texture_manager, model_manager, shader_manager, material_manager); }

	{ PhaseTimer t(tex_ms); LoadTextures (dir, texture_manager, engine_context); }
	{ PhaseTimer t(mdl_ms); LoadModels   (dir, model_manager); }
	{ PhaseTimer t(shd_ms); LoadShaders  (dir, shader_manager, model_manager, buffer_manager, texture_manager); }
	{ PhaseTimer t(mat_ms); LoadMaterials(dir, material_manager, texture_manager, shader_manager); }

	size_t loaded_count = 0;
	{
		{
			PhaseTimer t(clear_ms);
			SceneData* target = object_manager->GetScene(scene_name);
			if (target) target->clear();

			if (SceneData* prev_active = object_manager->GetActiveScene(); prev_active && prev_active != target)
				prev_active->clear();
		}

		{
			PhaseTimer t(ecs_ms);
			loaded_count = object_manager->LoadScene(scene_name, text).size();
		}

		if (SceneData* scene = object_manager->GetScene(scene_name)) {
			object_manager->SetActiveScene(scene_name);

			if (ui_yoga) ui_yoga->Reset();
		}
	}

	shader_manager->ReportOrphanCodeBindings();

	batch_builder->SetDirtyBatches(true);
	SDL_Log("LoadScene: loaded scene '%s' from '%s'", scene_name.c_str(), dir.c_str());
	SDL_Log("LoadScene TIMING [%zu ent, %.1f MB]: read=%.1f  wipe=%.1f  tex=%.1f  mdl=%.1f  shd=%.1f  mat=%.1f  clear=%.1f  ecs=%.1f  | total=%.1f ms",
		loaded_count, text.size() / (1024.0 * 1024.0),
		read_ms, wipe_ms, tex_ms, mdl_ms, shd_ms, mat_ms, clear_ms, ecs_ms,
		read_ms + wipe_ms + tex_ms + mdl_ms + shd_ms + mat_ms + clear_ms + ecs_ms);
}
