#include "PCH.h"
#include "ComponentSerializer.h"
#include <cstdio>     // snprintf
#include <cstdlib>    // strtof, strtol

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
//  Сериалайзеры встроенных компонентов
//  save: читает строку i из массива архетипа (тип знаем — он замкнут в функции).
//  load: ensure_component<T> + дописать значение (как одна ветка add_components).
// ============================================================
namespace {

float ParseFloat(const std::vector<std::string>& t, size_t k, float fallback = 0.0f)
{
    return k < t.size() ? std::strtof(t[k].c_str(), nullptr) : fallback;
}

// ---- Transform (Positions, SoA: 16 float, row-major x..l) ----
void SaveTransform(Archetype& arch, size_t i, std::string& out)
{
    const Positions& P = arch.get_array<Positions>()->data;
    char b[320];
    std::snprintf(b, sizeof b,
        "%.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g",
        P.x[i], P.y[i], P.z[i], P.w[i], P.a[i], P.b[i], P.c[i], P.d[i],
        P.e[i], P.f[i], P.g[i], P.h[i], P.i[i], P.j[i], P.k[i], P.l[i]);
    out = b;
}
void LoadTransform(Archetype& arch, const std::vector<std::string>& t)
{
    arch.ensure_component<Positions>();
    PositionProxy16 p{
        ParseFloat(t, 0, 1), ParseFloat(t, 1), ParseFloat(t, 2),  ParseFloat(t, 3),
        ParseFloat(t, 4),    ParseFloat(t, 5, 1), ParseFloat(t, 6), ParseFloat(t, 7),
        ParseFloat(t, 8),    ParseFloat(t, 9), ParseFloat(t, 10, 1), ParseFloat(t, 11),
        ParseFloat(t, 12),   ParseFloat(t, 13), ParseFloat(t, 14), ParseFloat(t, 15, 1) };
    arch.get_array<Positions>()->add(p);
}

// ---- Model (имя ассета) ----
void SaveModel(Archetype& arch, size_t i, std::string& out)
{
    out = (*arch.get_array<ModelComponent>())[i].name;
}
void LoadModel(Archetype& arch, const std::vector<std::string>& t)
{
    arch.ensure_component<ModelComponent>();
    ModelComponent m;
    m.model = nullptr;                 // указатель восстановит верхний слой по имени
    m.name  = t.empty() ? std::string{} : t[0];
    arch.get_array<ModelComponent>()->add(m);
}

// ---- Material (список имён, по одному на сабмеш) ----
void SaveMaterial(Archetype& arch, size_t i, std::string& out)
{
    const auto& names = (*arch.get_array<MaterialComponent>())[i].names;
    for (size_t k = 0; k < names.size(); ++k) {
        if (k) out += ' ';
        out += names[k];
    }
}
void LoadMaterial(Archetype& arch, const std::vector<std::string>& t)
{
    arch.ensure_component<MaterialComponent>();
    MaterialComponent mc;
    mc.names = t;                       // все токены — имена; указатели восстановит верхний слой
    arch.get_array<MaterialComponent>()->add(mc);
}

// ---- Draw (видимость + per-instance alpha/flags) ----
void SaveDraw(Archetype& arch, size_t i, std::string& out)
{
    const DrawComponent& d = (*arch.get_array<DrawComponent>())[i];
    char b[64];
    std::snprintf(b, sizeof b, "%d %.7g %u",
        d.visible ? 1 : 0, d.alpha, static_cast<unsigned>(d.flags));
    out = b;
}
void LoadDraw(Archetype& arch, const std::vector<std::string>& t)
{
    arch.ensure_component<DrawComponent>();
    DrawComponent d;
    d.visible = t.empty() || t[0] == "1";
    d.alpha   = ParseFloat(t, 1, 1.0f);
    d.flags   = t.size() > 2 ? static_cast<uint32_t>(std::strtoul(t[2].c_str(), nullptr, 10)) : 0u;
    arch.get_array<DrawComponent>()->add(d);
}

// ---- Shadow (тег, без данных) ----
void SaveShadow(Archetype&, size_t, std::string&) {}
void LoadShadow(Archetype& arch, const std::vector<std::string>&)
{
    arch.ensure_component<ShadowComponent>();
    arch.get_array<ShadowComponent>()->add(ShadowComponent{});
}

// ---- LocalMatrix (SoA LocalMatrices, 16 float column-major) ----
void SaveLocalMatrix(Archetype& arch, size_t i, std::string& out)
{
    const LocalMatrices& L = arch.get_array<LocalMatrices>()->data;
    char b[320];
    std::snprintf(b, sizeof b,
        "%.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g",
        L.m0[i], L.m1[i], L.m2[i], L.m3[i], L.m4[i], L.m5[i], L.m6[i], L.m7[i],
        L.m8[i], L.m9[i], L.m10[i], L.m11[i], L.m12[i], L.m13[i], L.m14[i], L.m15[i]);
    out = b;
}
void LoadLocalMatrix(Archetype& arch, const std::vector<std::string>& t)
{
    arch.ensure_component<LocalMatrices>();
    LocalMatrixProxy16 p{};
    for (int k = 0; k < 16; ++k)
        p.m[k] = ParseFloat(t, k, (k % 5 == 0) ? 1.0f : 0.0f);   // дефолт — единичная
    arch.get_array<LocalMatrices>()->add(p);
}

// ---- Parent (ссылка на родителя) ----
// save пишет СЫРОЙ id родителя — он уже хранится в компоненте, кросс-контекст не нужен.
// load кладёт его как есть (СТАРЫЙ id из файла); реальный ремап старый→новый + заполнение
// scene->children делает ObjectManager::LoadScene во втором проходе (родитель мог ещё не
// существовать в момент создания этой сущности).
void SaveParent(Archetype& arch, size_t i, std::string& out)
{
    out = std::to_string((*arch.get_array<ParentComponent>())[i].parent);
}
void LoadParent(Archetype& arch, const std::vector<std::string>& t)
{
    arch.ensure_component<ParentComponent>();
    ParentComponent p;
    p.parent = t.empty() ? 0u
        : static_cast<Entity>(std::strtoul(t[0].c_str(), nullptr, 10));
    arch.get_array<ParentComponent>()->add(p);
}

// ---- ShadowCaster (тег) ----
void SaveShadowCaster(Archetype&, size_t, std::string&) {}
void LoadShadowCaster(Archetype& arch, const std::vector<std::string>&)
{
    arch.ensure_component<ShadowCasterComponent>();
    arch.get_array<ShadowCasterComponent>()->add(ShadowCasterComponent{});
}

// ---- Лайты: пишем ТОЛЬКО входные поля. Приватные кэши (max_distance/cached_*) —
//      производные, пересчитаются лениво; needsUpdate=true заставит залить заново. ----
void SaveSpotLight(Archetype& arch, size_t i, std::string& out)
{
    const auto& d = (*arch.get_array<SpotLightComponent>())[i].light_data;
    char b[256];
    std::snprintf(b, sizeof b, "%.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g",
        d.source_radius, d.dir_x, d.dir_y, d.dir_z, d.source_angle,
        d.r, d.g, d.b, d.power, d.attenuation);
    out = b;
}
void LoadSpotLight(Archetype& arch, const std::vector<std::string>& t)
{
    arch.ensure_component<SpotLightComponent>();
    SpotLightComponent c;
    auto& d = c.light_data;
    d.source_radius = ParseFloat(t, 0);   d.dir_x = ParseFloat(t, 1); d.dir_y = ParseFloat(t, 2); d.dir_z = ParseFloat(t, 3, 1);
    d.source_angle  = ParseFloat(t, 4, 0.3f);
    d.r = ParseFloat(t, 5, 1); d.g = ParseFloat(t, 6, 1); d.b = ParseFloat(t, 7, 1);
    d.power = ParseFloat(t, 8, 1); d.attenuation = ParseFloat(t, 9, 1);
    c.needsUpdate = true;
    arch.get_array<SpotLightComponent>()->add(c);
}

void SaveSphereLight(Archetype& arch, size_t i, std::string& out)
{
    const auto& d = (*arch.get_array<SphereLightComponent>())[i].light_data;
    char b[160];
    std::snprintf(b, sizeof b, "%.7g %.7g %.7g %.7g %.7g %.7g",
        d.source_radius, d.r, d.g, d.b, d.power, d.attenuation);
    out = b;
}
void LoadSphereLight(Archetype& arch, const std::vector<std::string>& t)
{
    arch.ensure_component<SphereLightComponent>();
    SphereLightComponent c;
    auto& d = c.light_data;
    d.source_radius = ParseFloat(t, 0);
    d.r = ParseFloat(t, 1, 1); d.g = ParseFloat(t, 2, 1); d.b = ParseFloat(t, 3, 1);
    d.power = ParseFloat(t, 4, 1); d.attenuation = ParseFloat(t, 5, 1);
    c.needsUpdate = true;
    arch.get_array<SphereLightComponent>()->add(c);
}

void SaveDirectLight(Archetype& arch, size_t i, std::string& out)
{
    const auto& d = (*arch.get_array<DirectLightComponent>())[i].light_data;
    char b[320];
    std::snprintf(b, sizeof b,
        "%.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g %d %.7g",
        d.dir_x, d.dir_y, d.dir_z, d.r, d.g, d.b, d.power,
        d.center_x, d.center_y, d.center_z, d.half_extent, d.half_depth,
        d.cascade_count, d.cascade_ratio);
    out = b;
}
void LoadDirectLight(Archetype& arch, const std::vector<std::string>& t)
{
    arch.ensure_component<DirectLightComponent>();
    DirectLightComponent c;
    auto& d = c.light_data;
    d.dir_x = ParseFloat(t, 0); d.dir_y = ParseFloat(t, 1, -1); d.dir_z = ParseFloat(t, 2);
    d.r = ParseFloat(t, 3, 1); d.g = ParseFloat(t, 4, 1); d.b = ParseFloat(t, 5, 1);
    d.power = ParseFloat(t, 6, 1);
    d.center_x = ParseFloat(t, 7); d.center_y = ParseFloat(t, 8); d.center_z = ParseFloat(t, 9);
    d.half_extent = ParseFloat(t, 10, 20.0f); d.half_depth = ParseFloat(t, 11, 20.0f);
    int cc = t.size() > 12 ? std::atoi(t[12].c_str()) : 3;
    if (cc < 1) cc = 1;
    if (cc > DirectLightComponent::DirectLightData::MAX_CASCADES)
        cc = DirectLightComponent::DirectLightData::MAX_CASCADES;
    d.cascade_count = cc;
    d.cascade_ratio = ParseFloat(t, 13, 3.0f);
    c.needsUpdate = true;
    arch.get_array<DirectLightComponent>()->add(c);
}

// ---- Mass ----
void SaveMass(Archetype& arch, size_t i, std::string& out)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.7g", (*arch.get_array<MassComponent>())[i].mass);
    out = b;
}
void LoadMass(Archetype& arch, const std::vector<std::string>& t)
{
    arch.ensure_component<MassComponent>();
    MassComponent m{};
    m.mass = ParseFloat(t, 0);
    arch.get_array<MassComponent>()->add(m);
}

// ---- EditorHidden (тег: скрыт из списка UI; персистится как авторское состояние) ----
void SaveEditorHidden(Archetype&, size_t, std::string&) {}
void LoadEditorHidden(Archetype& arch, const std::vector<std::string>&)
{
    arch.ensure_component<EditorHiddenComponent>();
    arch.get_array<EditorHiddenComponent>()->add(EditorHiddenComponent{});
}

// ---- Velocity / Acceleration (SoA, рантайм-стейт; сериализуем как начальные значения) ----
void SaveVelocity(Archetype& arch, size_t i, std::string& out)
{
    const Velocities& V = arch.get_array<Velocities>()->data;
    char b[64];
    std::snprintf(b, sizeof b, "%.7g %.7g %.7g", V.x[i], V.y[i], V.z[i]);
    out = b;
}
void LoadVelocity(Archetype& arch, const std::vector<std::string>& t)
{
    arch.ensure_component<Velocities>();
    VelocityProxy p{ ParseFloat(t, 0), ParseFloat(t, 1), ParseFloat(t, 2) };
    arch.get_array<Velocities>()->add(p);
}

void SaveAcceleration(Archetype& arch, size_t i, std::string& out)
{
    const Accelerations& A = arch.get_array<Accelerations>()->data;
    char b[64];
    std::snprintf(b, sizeof b, "%.7g %.7g %.7g", A.x[i], A.y[i], A.z[i]);
    out = b;
}
void LoadAcceleration(Archetype& arch, const std::vector<std::string>& t)
{
    arch.ensure_component<Accelerations>();
    AccelerationProxy p{ ParseFloat(t, 0), ParseFloat(t, 1), ParseFloat(t, 2) };
    arch.get_array<Accelerations>()->add(p);
}

} // namespace

void RegisterBuiltinComponentSerializers()
{
    auto& reg = ComponentSerializerRegistry::Get();
    reg.Register({ "Transform", std::type_index(typeid(Positions)),         &SaveTransform, &LoadTransform });
    reg.Register({ "Model",     std::type_index(typeid(ModelComponent)),    &SaveModel,     &LoadModel });
    reg.Register({ "Material",  std::type_index(typeid(MaterialComponent)), &SaveMaterial,  &LoadMaterial });
    reg.Register({ "Draw",        std::type_index(typeid(DrawComponent)),       &SaveDraw,        &LoadDraw });
    reg.Register({ "Shadow",      std::type_index(typeid(ShadowComponent)),     &SaveShadow,      &LoadShadow });
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
