#include "PCH.h"
#include "ComponentSerializer.h"
#include <cstdio>     // snprintf — имена колонок m0..m15
#include <string>
#include <vector>

// ============================================================
//  Реестр
// ============================================================
ComponentSerializerRegistry& ComponentSerializerRegistry::Get()
{
    static ComponentSerializerRegistry instance;
    return instance;
}

void ComponentSerializerRegistry::Register(ComponentSerializer s)
{
    if (by_name_.count(s.name)) return;   // идемпотентно: уже зарегали
    const size_t idx = serializers_.size();
    by_name_[s.name]     = idx;
    by_type_[s.sig_type] = idx;
    serializers_.push_back(std::move(s));
}

const ComponentSerializer* ComponentSerializerRegistry::ByName(const std::string& name) const
{
    auto it = by_name_.find(name);
    return it == by_name_.end() ? nullptr : &serializers_[it->second];
}

const ComponentSerializer* ComponentSerializerRegistry::ByType(std::type_index t) const
{
    auto it = by_type_.find(t);
    return it == by_type_.end() ? nullptr : &serializers_[it->second];
}

// ============================================================
//  Колоночные сериалайзеры компонентов (scene.json)
//  save: пишет ВЕСЬ компонент архетипа колонками по полям (count строк).
//  load: ensure_component<T> + РОВНО count add'ов, читая колонки по полю.
// ============================================================
namespace {

// ---- save: колонка из SoA-массива поля (P.member[] — уже готовая колонка) ----
#define SOA_COL(P, member) do { \
    yyjson_mut_val* _a = yyjson_mut_obj_add_arr(doc, comp, #member); \
    for (size_t _i = 0; _i < count; ++_i) yyjson_mut_arr_add_real(doc, _a, (double)(P).member[_i]); \
} while (0)

// ---- save: колонка AoS-поля (expr использует локальный `arr` и индекс `_i`) ----
#define AOS_R(name, expr) do { \
    yyjson_mut_val* _a = yyjson_mut_obj_add_arr(doc, comp, name); \
    for (size_t _i = 0; _i < count; ++_i) yyjson_mut_arr_add_real(doc, _a, (double)(expr)); \
} while (0)
#define AOS_U(name, expr) do { \
    yyjson_mut_val* _a = yyjson_mut_obj_add_arr(doc, comp, name); \
    for (size_t _i = 0; _i < count; ++_i) yyjson_mut_arr_add_uint(doc, _a, (uint64_t)(expr)); \
} while (0)
#define AOS_B(name, expr) do { \
    yyjson_mut_val* _a = yyjson_mut_obj_add_arr(doc, comp, name); \
    for (size_t _i = 0; _i < count; ++_i) yyjson_mut_arr_add_bool(doc, _a, (bool)(expr)); \
} while (0)

// ---- load: число из json (int/uint/real/bool) с фолбэком ----
double GetNum(yyjson_val* v, double fb)
{
    if (!v) return fb;
    if (yyjson_is_real(v)) return yyjson_get_real(v);
    if (yyjson_is_sint(v)) return (double)yyjson_get_sint(v);
    if (yyjson_is_uint(v)) return (double)yyjson_get_uint(v);
    if (yyjson_is_bool(v)) return yyjson_get_bool(v) ? 1.0 : 0.0;
    return fb;
}

// ---- load: колонка → out[0..count). count-гвард: нет/короче → fb, длиннее → усечь. ----
void ReadReal(yyjson_val* comp, const char* name, size_t n, float fb, std::vector<float>& out)
{
    out.assign(n, fb);
    yyjson_val* col = comp ? yyjson_obj_get(comp, name) : nullptr;
    if (!col) return;
    size_t idx, max; yyjson_val* v;
    yyjson_arr_foreach(col, idx, max, v) { if (idx >= n) break; out[idx] = (float)GetNum(v, fb); }
}
void ReadUint(yyjson_val* comp, const char* name, size_t n, uint32_t fb, std::vector<uint32_t>& out)
{
    out.assign(n, fb);
    yyjson_val* col = comp ? yyjson_obj_get(comp, name) : nullptr;
    if (!col) return;
    size_t idx, max; yyjson_val* v;
    yyjson_arr_foreach(col, idx, max, v) { if (idx >= n) break; out[idx] = (uint32_t)GetNum(v, fb); }
}
void ReadStr(yyjson_val* comp, const char* name, size_t n, std::vector<std::string>& out)
{
    out.assign(n, std::string{});
    yyjson_val* col = comp ? yyjson_obj_get(comp, name) : nullptr;
    if (!col) return;
    size_t idx, max; yyjson_val* v;
    yyjson_arr_foreach(col, idx, max, v) { if (idx >= n) break; const char* s = yyjson_get_str(v); if (s) out[idx] = s; }
}

// ---- Transform (Positions, SoA: 16 float, row-major x..l) ----
void SaveTransform(Archetype& arch, size_t count, yyjson_mut_doc* doc, yyjson_mut_val* comp)
{
    const Positions& P = arch.get_array<Positions>()->data;
    SOA_COL(P, x); SOA_COL(P, y); SOA_COL(P, z); SOA_COL(P, w);
    SOA_COL(P, a); SOA_COL(P, b); SOA_COL(P, c); SOA_COL(P, d);
    SOA_COL(P, e); SOA_COL(P, f); SOA_COL(P, g); SOA_COL(P, h);
    SOA_COL(P, i); SOA_COL(P, j); SOA_COL(P, k); SOA_COL(P, l);
}
void LoadTransform(Archetype& arch, yyjson_val* comp, size_t count)
{
    arch.ensure_component<Positions>();
    static const char* N[16] = { "x","y","z","w","a","b","c","d","e","f","g","h","i","j","k","l" };
    static const float D[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };   // единичная (row-major)
    std::vector<float> c[16];
    for (int k = 0; k < 16; ++k) ReadReal(comp, N[k], count, D[k], c[k]);
    auto* pos = arch.get_array<Positions>();
    for (size_t i = 0; i < count; ++i)
        pos->add(PositionProxy16{ c[0][i],c[1][i],c[2][i],c[3][i],  c[4][i],c[5][i],c[6][i],c[7][i],
                                  c[8][i],c[9][i],c[10][i],c[11][i], c[12][i],c[13][i],c[14][i],c[15][i] });
}

// ---- Model (имя ассета) ----
void SaveModel(Archetype& arch, size_t count, yyjson_mut_doc* doc, yyjson_mut_val* comp)
{
    auto& arr = *arch.get_array<ModelComponent>();
    yyjson_mut_val* a = yyjson_mut_obj_add_arr(doc, comp, "name");
    for (size_t _i = 0; _i < count; ++_i) yyjson_mut_arr_add_strcpy(doc, a, arr[_i].name.c_str());
}
void LoadModel(Archetype& arch, yyjson_val* comp, size_t count)
{
    arch.ensure_component<ModelComponent>();
    std::vector<std::string> names; ReadStr(comp, "name", count, names);
    auto* a = arch.get_array<ModelComponent>();
    for (size_t i = 0; i < count; ++i) {
        ModelComponent m; m.model = nullptr; m.name = std::move(names[i]);   // указатель восстановит верхний слой
        a->add(m);
    }
}

// ---- Material (список имён на сущность → зубчатая колонка array-of-arrays) ----
void SaveMaterial(Archetype& arch, size_t count, yyjson_mut_doc* doc, yyjson_mut_val* comp)
{
    auto& arr = *arch.get_array<MaterialComponent>();
    yyjson_mut_val* col = yyjson_mut_obj_add_arr(doc, comp, "names");
    for (size_t _i = 0; _i < count; ++_i) {
        yyjson_mut_val* row = yyjson_mut_arr_add_arr(doc, col);
        for (const auto& nm : arr[_i].names) yyjson_mut_arr_add_strcpy(doc, row, nm.c_str());
    }
}
void LoadMaterial(Archetype& arch, yyjson_val* comp, size_t count)
{
    arch.ensure_component<MaterialComponent>();
    std::vector<MaterialComponent> rows(count);
    yyjson_val* col = comp ? yyjson_obj_get(comp, "names") : nullptr;
    if (col) {
        size_t idx, max; yyjson_val* row;
        yyjson_arr_foreach(col, idx, max, row) {
            if (idx >= count) break;
            size_t j, jm; yyjson_val* s;
            yyjson_arr_foreach(row, j, jm, s) { const char* str = yyjson_get_str(s); if (str) rows[idx].names.emplace_back(str); }
        }
    }
    auto* a = arch.get_array<MaterialComponent>();
    for (size_t i = 0; i < count; ++i) a->add(rows[i]);   // указатели восстановит верхний слой
}

// ---- Draw (видимость + per-instance alpha/flags) ----
void SaveDraw(Archetype& arch, size_t count, yyjson_mut_doc* doc, yyjson_mut_val* comp)
{
    auto& arr = *arch.get_array<DrawComponent>();
    AOS_B("visible", arr[_i].visible);
    AOS_R("alpha",   arr[_i].alpha);
    AOS_U("flags",   arr[_i].flags);
}
void LoadDraw(Archetype& arch, yyjson_val* comp, size_t count)
{
    arch.ensure_component<DrawComponent>();
    std::vector<uint32_t> vis, flags; std::vector<float> alpha;
    ReadUint(comp, "visible", count, 1u, vis);
    ReadReal(comp, "alpha", count, 1.0f, alpha);
    ReadUint(comp, "flags", count, 0u, flags);
    auto* a = arch.get_array<DrawComponent>();
    for (size_t i = 0; i < count; ++i) { DrawComponent d; d.visible = vis[i] != 0; d.alpha = alpha[i]; d.flags = flags[i]; a->add(d); }
}

// ---- Shadow (тег, без данных) ----
void SaveShadow(Archetype&, size_t, yyjson_mut_doc*, yyjson_mut_val*) {}
void LoadShadow(Archetype& arch, yyjson_val*, size_t count)
{
    arch.ensure_component<ShadowComponent>();
    auto* a = arch.get_array<ShadowComponent>();
    for (size_t i = 0; i < count; ++i) a->add(ShadowComponent{});
}

// ---- LocalMatrix (SoA LocalMatrices, 16 float column-major m0..m15) ----
void SaveLocalMatrix(Archetype& arch, size_t count, yyjson_mut_doc* doc, yyjson_mut_val* comp)
{
    const LocalMatrices& L = arch.get_array<LocalMatrices>()->data;
    SOA_COL(L, m0); SOA_COL(L, m1); SOA_COL(L, m2);  SOA_COL(L, m3);
    SOA_COL(L, m4); SOA_COL(L, m5); SOA_COL(L, m6);  SOA_COL(L, m7);
    SOA_COL(L, m8); SOA_COL(L, m9); SOA_COL(L, m10); SOA_COL(L, m11);
    SOA_COL(L, m12); SOA_COL(L, m13); SOA_COL(L, m14); SOA_COL(L, m15);
}
void LoadLocalMatrix(Archetype& arch, yyjson_val* comp, size_t count)
{
    arch.ensure_component<LocalMatrices>();
    std::vector<float> c[16];
    char nm[8];
    for (int k = 0; k < 16; ++k) { std::snprintf(nm, sizeof nm, "m%d", k); ReadReal(comp, nm, count, (k % 5 == 0) ? 1.0f : 0.0f, c[k]); }
    auto* lm = arch.get_array<LocalMatrices>();
    for (size_t i = 0; i < count; ++i) { LocalMatrixProxy16 p; for (int k = 0; k < 16; ++k) p.m[k] = c[k][i]; lm->add(p); }
}

// ---- Parent (СЫРОЙ старый id из файла; ремап old→new делает ObjectManager::LoadScene, проход 2) ----
void SaveParent(Archetype& arch, size_t count, yyjson_mut_doc* doc, yyjson_mut_val* comp)
{
    auto& arr = *arch.get_array<ParentComponent>();
    AOS_U("parent", arr[_i].parent);
}
void LoadParent(Archetype& arch, yyjson_val* comp, size_t count)
{
    arch.ensure_component<ParentComponent>();
    std::vector<uint32_t> par; ReadUint(comp, "parent", count, 0u, par);
    auto* a = arch.get_array<ParentComponent>();
    for (size_t i = 0; i < count; ++i) { ParentComponent p; p.parent = static_cast<Entity>(par[i]); a->add(p); }
}

// ---- ShadowCaster (тег) ----
void SaveShadowCaster(Archetype&, size_t, yyjson_mut_doc*, yyjson_mut_val*) {}
void LoadShadowCaster(Archetype& arch, yyjson_val*, size_t count)
{
    arch.ensure_component<ShadowCasterComponent>();
    auto* a = arch.get_array<ShadowCasterComponent>();
    for (size_t i = 0; i < count; ++i) a->add(ShadowCasterComponent{});
}

// ---- Лайты: только входные поля; приватные кэши (max_distance/cached_*) пересчитаются, needsUpdate=true ----
void SaveSpotLight(Archetype& arch, size_t count, yyjson_mut_doc* doc, yyjson_mut_val* comp)
{
    auto& arr = *arch.get_array<SpotLightComponent>();
    AOS_R("source_radius", arr[_i].light_data.source_radius);
    AOS_R("dir_x", arr[_i].light_data.dir_x); AOS_R("dir_y", arr[_i].light_data.dir_y); AOS_R("dir_z", arr[_i].light_data.dir_z);
    AOS_R("source_angle", arr[_i].light_data.source_angle);
    AOS_R("r", arr[_i].light_data.r); AOS_R("g", arr[_i].light_data.g); AOS_R("b", arr[_i].light_data.b);
    AOS_R("power", arr[_i].light_data.power); AOS_R("attenuation", arr[_i].light_data.attenuation);
}
void LoadSpotLight(Archetype& arch, yyjson_val* comp, size_t count)
{
    arch.ensure_component<SpotLightComponent>();
    std::vector<float> sr, dx, dy, dz, sa, r, g, b, pw, at;
    ReadReal(comp, "source_radius", count, 0, sr);
    ReadReal(comp, "dir_x", count, 0, dx); ReadReal(comp, "dir_y", count, 0, dy); ReadReal(comp, "dir_z", count, 1, dz);
    ReadReal(comp, "source_angle", count, 0.3f, sa);
    ReadReal(comp, "r", count, 1, r); ReadReal(comp, "g", count, 1, g); ReadReal(comp, "b", count, 1, b);
    ReadReal(comp, "power", count, 1, pw); ReadReal(comp, "attenuation", count, 1, at);
    auto* a = arch.get_array<SpotLightComponent>();
    for (size_t i = 0; i < count; ++i) {
        SpotLightComponent c; auto& d = c.light_data;
        d.source_radius = sr[i]; d.dir_x = dx[i]; d.dir_y = dy[i]; d.dir_z = dz[i]; d.source_angle = sa[i];
        d.r = r[i]; d.g = g[i]; d.b = b[i]; d.power = pw[i]; d.attenuation = at[i];
        c.needsUpdate = true; a->add(c);
    }
}

void SaveSphereLight(Archetype& arch, size_t count, yyjson_mut_doc* doc, yyjson_mut_val* comp)
{
    auto& arr = *arch.get_array<SphereLightComponent>();
    AOS_R("source_radius", arr[_i].light_data.source_radius);
    AOS_R("r", arr[_i].light_data.r); AOS_R("g", arr[_i].light_data.g); AOS_R("b", arr[_i].light_data.b);
    AOS_R("power", arr[_i].light_data.power); AOS_R("attenuation", arr[_i].light_data.attenuation);
}
void LoadSphereLight(Archetype& arch, yyjson_val* comp, size_t count)
{
    arch.ensure_component<SphereLightComponent>();
    std::vector<float> sr, r, g, b, pw, at;
    ReadReal(comp, "source_radius", count, 0, sr);
    ReadReal(comp, "r", count, 1, r); ReadReal(comp, "g", count, 1, g); ReadReal(comp, "b", count, 1, b);
    ReadReal(comp, "power", count, 1, pw); ReadReal(comp, "attenuation", count, 1, at);
    auto* a = arch.get_array<SphereLightComponent>();
    for (size_t i = 0; i < count; ++i) {
        SphereLightComponent c; auto& d = c.light_data;
        d.source_radius = sr[i]; d.r = r[i]; d.g = g[i]; d.b = b[i]; d.power = pw[i]; d.attenuation = at[i];
        c.needsUpdate = true; a->add(c);
    }
}

void SaveDirectLight(Archetype& arch, size_t count, yyjson_mut_doc* doc, yyjson_mut_val* comp)
{
    auto& arr = *arch.get_array<DirectLightComponent>();
    AOS_R("dir_x", arr[_i].light_data.dir_x); AOS_R("dir_y", arr[_i].light_data.dir_y); AOS_R("dir_z", arr[_i].light_data.dir_z);
    AOS_R("r", arr[_i].light_data.r); AOS_R("g", arr[_i].light_data.g); AOS_R("b", arr[_i].light_data.b);
    AOS_R("power", arr[_i].light_data.power);
    AOS_R("center_x", arr[_i].light_data.center_x); AOS_R("center_y", arr[_i].light_data.center_y); AOS_R("center_z", arr[_i].light_data.center_z);
    AOS_R("half_extent", arr[_i].light_data.half_extent); AOS_R("half_depth", arr[_i].light_data.half_depth);
    AOS_U("cascade_count", arr[_i].light_data.cascade_count);
    AOS_R("cascade_ratio", arr[_i].light_data.cascade_ratio);
}
void LoadDirectLight(Archetype& arch, yyjson_val* comp, size_t count)
{
    arch.ensure_component<DirectLightComponent>();
    std::vector<float> dx, dy, dz, r, g, b, pw, cx, cy, cz, he, hd, cr;
    std::vector<uint32_t> cc;
    ReadReal(comp, "dir_x", count, 0, dx); ReadReal(comp, "dir_y", count, -1, dy); ReadReal(comp, "dir_z", count, 0, dz);
    ReadReal(comp, "r", count, 1, r); ReadReal(comp, "g", count, 1, g); ReadReal(comp, "b", count, 1, b);
    ReadReal(comp, "power", count, 1, pw);
    ReadReal(comp, "center_x", count, 0, cx); ReadReal(comp, "center_y", count, 0, cy); ReadReal(comp, "center_z", count, 0, cz);
    ReadReal(comp, "half_extent", count, 20.0f, he); ReadReal(comp, "half_depth", count, 20.0f, hd);
    ReadUint(comp, "cascade_count", count, 3u, cc);
    ReadReal(comp, "cascade_ratio", count, 3.0f, cr);
    auto* a = arch.get_array<DirectLightComponent>();
    for (size_t i = 0; i < count; ++i) {
        DirectLightComponent c; auto& d = c.light_data;
        d.dir_x = dx[i]; d.dir_y = dy[i]; d.dir_z = dz[i]; d.r = r[i]; d.g = g[i]; d.b = b[i]; d.power = pw[i];
        d.center_x = cx[i]; d.center_y = cy[i]; d.center_z = cz[i]; d.half_extent = he[i]; d.half_depth = hd[i];
        int cci = (int)cc[i];
        if (cci < 1) cci = 1;
        if (cci > DirectLightComponent::DirectLightData::MAX_CASCADES) cci = DirectLightComponent::DirectLightData::MAX_CASCADES;
        d.cascade_count = cci; d.cascade_ratio = cr[i];
        c.needsUpdate = true; a->add(c);
    }
}

// ---- Mass ----
void SaveMass(Archetype& arch, size_t count, yyjson_mut_doc* doc, yyjson_mut_val* comp)
{
    auto& arr = *arch.get_array<MassComponent>();
    AOS_R("mass", arr[_i].mass);
}
void LoadMass(Archetype& arch, yyjson_val* comp, size_t count)
{
    arch.ensure_component<MassComponent>();
    std::vector<float> m; ReadReal(comp, "mass", count, 0, m);
    auto* a = arch.get_array<MassComponent>();
    for (size_t i = 0; i < count; ++i) { MassComponent mc{}; mc.mass = m[i]; a->add(mc); }
}

// ---- EditorHidden (тег: скрыт из списка UI; персистится как авторское состояние) ----
void SaveEditorHidden(Archetype&, size_t, yyjson_mut_doc*, yyjson_mut_val*) {}
void LoadEditorHidden(Archetype& arch, yyjson_val*, size_t count)
{
    arch.ensure_component<EditorHiddenComponent>();
    auto* a = arch.get_array<EditorHiddenComponent>();
    for (size_t i = 0; i < count; ++i) a->add(EditorHiddenComponent{});
}

// ---- Velocity / Acceleration (SoA x,y,z) ----
void SaveVelocity(Archetype& arch, size_t count, yyjson_mut_doc* doc, yyjson_mut_val* comp)
{
    const Velocities& V = arch.get_array<Velocities>()->data;
    SOA_COL(V, x); SOA_COL(V, y); SOA_COL(V, z);
}
void LoadVelocity(Archetype& arch, yyjson_val* comp, size_t count)
{
    arch.ensure_component<Velocities>();
    std::vector<float> x, y, z; ReadReal(comp, "x", count, 0, x); ReadReal(comp, "y", count, 0, y); ReadReal(comp, "z", count, 0, z);
    auto* a = arch.get_array<Velocities>();
    for (size_t i = 0; i < count; ++i) a->add(VelocityProxy{ x[i], y[i], z[i] });
}

void SaveAcceleration(Archetype& arch, size_t count, yyjson_mut_doc* doc, yyjson_mut_val* comp)
{
    const Accelerations& A = arch.get_array<Accelerations>()->data;
    SOA_COL(A, x); SOA_COL(A, y); SOA_COL(A, z);
}
void LoadAcceleration(Archetype& arch, yyjson_val* comp, size_t count)
{
    arch.ensure_component<Accelerations>();
    std::vector<float> x, y, z; ReadReal(comp, "x", count, 0, x); ReadReal(comp, "y", count, 0, y); ReadReal(comp, "z", count, 0, z);
    auto* a = arch.get_array<Accelerations>();
    for (size_t i = 0; i < count; ++i) a->add(AccelerationProxy{ x[i], y[i], z[i] });
}

#undef SOA_COL
#undef AOS_R
#undef AOS_U
#undef AOS_B

} // namespace

void RegisterBuiltinComponentSerializers()
{
    auto& reg = ComponentSerializerRegistry::Get();
    reg.Register({ "Transform",    std::type_index(typeid(Positions)),            &SaveTransform,    &LoadTransform });
    reg.Register({ "Model",        std::type_index(typeid(ModelComponent)),       &SaveModel,        &LoadModel });
    reg.Register({ "Material",     std::type_index(typeid(MaterialComponent)),    &SaveMaterial,     &LoadMaterial });
    reg.Register({ "Draw",         std::type_index(typeid(DrawComponent)),        &SaveDraw,         &LoadDraw });
    reg.Register({ "Shadow",       std::type_index(typeid(ShadowComponent)),      &SaveShadow,       &LoadShadow });
    reg.Register({ "LocalMatrix",  std::type_index(typeid(LocalMatrices)),        &SaveLocalMatrix,  &LoadLocalMatrix });
    reg.Register({ "Parent",       std::type_index(typeid(ParentComponent)),      &SaveParent,       &LoadParent });
    reg.Register({ "ShadowCaster", std::type_index(typeid(ShadowCasterComponent)),&SaveShadowCaster, &LoadShadowCaster });
    reg.Register({ "SpotLight",    std::type_index(typeid(SpotLightComponent)),   &SaveSpotLight,    &LoadSpotLight });
    reg.Register({ "SphereLight",  std::type_index(typeid(SphereLightComponent)), &SaveSphereLight,  &LoadSphereLight });
    reg.Register({ "DirectLight",  std::type_index(typeid(DirectLightComponent)), &SaveDirectLight,  &LoadDirectLight });
    reg.Register({ "Mass",         std::type_index(typeid(MassComponent)),        &SaveMass,         &LoadMass });
    reg.Register({ "EditorHidden", std::type_index(typeid(EditorHiddenComponent)),&SaveEditorHidden, &LoadEditorHidden });
    reg.Register({ "Velocity",     std::type_index(typeid(Velocities)),           &SaveVelocity,     &LoadVelocity });
    reg.Register({ "Acceleration", std::type_index(typeid(Accelerations)),        &SaveAcceleration, &LoadAcceleration });
}
