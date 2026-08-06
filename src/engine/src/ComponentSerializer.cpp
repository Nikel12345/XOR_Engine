#include "PCH.h"
#include "ComponentSerializer.h"
#include <cfloat>
#include <string>
#include <vector>

ComponentSpecRegistry& ComponentSpecRegistry::Get()
{
    static ComponentSpecRegistry instance;
    return instance;
}

void ComponentSpecRegistry::Register(ComponentSpec s)
{
    if (by_name_.count(s.name)) return;   // идемпотентно: уже зарегали
    const size_t idx = specs_.size();
    by_name_[s.name]     = idx;
    by_type_[s.sig_type] = idx;
    specs_.push_back(std::move(s));
}

const ComponentSpec* ComponentSpecRegistry::ByName(const std::string& name) const
{
    auto it = by_name_.find(name);
    return it == by_name_.end() ? nullptr : &specs_[it->second];
}

const ComponentSpec* ComponentSpecRegistry::ByType(std::type_index t) const
{
    auto it = by_type_.find(t);
    return it == by_type_.end() ? nullptr : &specs_[it->second];
}

//  FieldSpec — фабрики (агрегат с парой get/set по виду поля)
FieldSpec FieldSpec::Num(const char* key, FieldKind kind,
                         double (*get)(Archetype&, size_t), void (*set)(Archetype&, size_t, double),
                         float lo, float hi, float speed)
{
    FieldSpec f;
    f.key = key; f.kind = kind;
    f.get_num = get; f.set_num = set;
    f.lo = lo; f.hi = hi; f.speed = speed;
    return f;
}

FieldSpec FieldSpec::Str(const char* key,
                         const std::string& (*get)(Archetype&, size_t), void (*set)(Archetype&, size_t, std::string),
                         FieldKind kind)
{
    FieldSpec f;
    f.key = key; f.kind = kind;
    f.get_str = get; f.set_str = set;
    return f;
}

//  Генераторы: интерпретация схемы полей (scene.json колоночный)
namespace {

// Число из json (real/int/uint/bool). Не-число → false: поле строки остаётся дефолтным.
bool TryGetNum(yyjson_val* v, double& out)
{
    if (yyjson_is_real(v)) { out = yyjson_get_real(v); return true; }
    if (yyjson_is_sint(v)) { out = (double)yyjson_get_sint(v); return true; }
    if (yyjson_is_uint(v)) { out = (double)yyjson_get_uint(v); return true; }
    if (yyjson_is_bool(v)) { out = yyjson_get_bool(v) ? 1.0 : 0.0; return true; }
    return false;
}

} // namespace

void ComponentSpec::Save(Archetype& arch, size_t count, yyjson_mut_doc* doc, yyjson_mut_val* comp) const
{
    if (custom_save) { custom_save(arch, count, doc, comp); return; }
    for (const FieldSpec& f : fields) {
        yyjson_mut_val* col = yyjson_mut_obj_add_arr(doc, comp, f.key);
        switch (f.kind) {
        case FieldKind::F32:
        case FieldKind::Angle:   // в файле — те же радианы-real, градусы только в слайдере UI
            for (size_t i = 0; i < count; ++i) yyjson_mut_arr_add_real(doc, col, f.get_num(arch, i));
            break;
        case FieldKind::U32:
            for (size_t i = 0; i < count; ++i) yyjson_mut_arr_add_uint(doc, col, (uint64_t)f.get_num(arch, i));
            break;
        case FieldKind::Bool:
            for (size_t i = 0; i < count; ++i) yyjson_mut_arr_add_bool(doc, col, f.get_num(arch, i) != 0.0);
            break;
        default:   // Str / Asset*
            for (size_t i = 0; i < count; ++i) yyjson_mut_arr_add_strcpy(doc, col, f.get_str(arch, i).c_str());
            break;
        }
    }
}

void ComponentSpec::Load(Archetype& arch, yyjson_val* comp, size_t count) const
{
    if (custom_load) { custom_load(arch, comp, count); return; }
    for (size_t i = 0; i < count; ++i) add_default(arch);     // дефолты = member-инициализаторы T{}
    if (!comp) return;                                        // компонент без данных (тег) / форма создания
    const size_t base = arch.entities.size() - count;         // строки дописаны в хвост (инвариант в .h)
    for (const FieldSpec& f : fields) {
        yyjson_val* col = yyjson_obj_get(comp, f.key);
        if (!col) continue;                                   // нет колонки → дефолт
        size_t idx, max; yyjson_val* v;
        if (f.set_str) {
            yyjson_arr_foreach(col, idx, max, v) {
                if (idx >= count) break;                      // count-гвард: длиннее → усечь
                if (const char* s = yyjson_get_str(v)) f.set_str(arch, base + idx, s);
            }
        }
        else {
            yyjson_arr_foreach(col, idx, max, v) {
                if (idx >= count) break;
                double d;
                if (!TryGetNum(v, d)) continue;
                if (f.clamp_on_load) d = d < f.lo ? f.lo : (d > f.hi ? f.hi : d);
                f.set_num(arch, base + idx, d);
            }
        }
    }
}

//  Аксессоры полей: пара (get, set) каптурлесс-лямбдами.
//  AoS — поле по цепочке членов компонента T; SoA — колонка col хранилища S.
//  set приводит double к фактическому типу поля (float/int/uint32_t/bool).
#define AOS_NUM(T, path) \
    [](Archetype& a, size_t i) -> double { return (double)(*a.get_array<T>())[i].path; }, \
    [](Archetype& a, size_t i, double v) { auto& r = (*a.get_array<T>())[i].path; r = (std::decay_t<decltype(r)>)v; }
#define SOA_NUM(S, col) \
    [](Archetype& a, size_t i) -> double { return (double)a.get_array<S>()->data.col[i]; }, \
    [](Archetype& a, size_t i, double v) { auto& r = a.get_array<S>()->data.col[i]; r = (std::decay_t<decltype(r)>)v; }
#define AOS_STR(T, path) \
    [](Archetype& a, size_t i) -> const std::string& { return (*a.get_array<T>())[i].path; }, \
    [](Archetype& a, size_t i, std::string v) { (*a.get_array<T>())[i].path = std::move(v); }

//  Material — escape hatch: список имён на сущность не колонка одного поля,
//  а зубчатый массив array-of-arrays. Схемой не выражается — рукописная пара.
namespace {

void SaveMaterial(Archetype& arch, size_t count, yyjson_mut_doc* doc, yyjson_mut_val* comp)
{
    auto& arr = *arch.get_array<MaterialComponent>();
    yyjson_mut_val* col = yyjson_mut_obj_add_arr(doc, comp, "names");
    for (size_t i = 0; i < count; ++i) {
        yyjson_mut_val* row = yyjson_mut_arr_add_arr(doc, col);
        for (const auto& nm : arr[i].names) yyjson_mut_arr_add_strcpy(doc, row, nm.c_str());
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
    for (size_t i = 0; i < count; ++i) a->add(rows[i]);
}

} // namespace

//  Регистрация встроенных компонентов: вся правда о компоненте — одна запись.
//  Порядок fields = порядок колонок в файле (сохраняем прежний формат байт-в-байт).
void RegisterBuiltinComponentSpecs()
{
    using enum FieldKind;
    auto& reg = ComponentSpecRegistry::Get();

    // ---- Transform: SoA Positions, 16 колонок x..l (row-major, дефолт — единичная из Proxy) ----
    reg.Register({ .name = "Transform", .sig_type = typeid(Positions),
        .add_default = AddDefaultSoA<Positions, PositionProxy16>,
        .fields = {
            FieldSpec::Num("x", F32, SOA_NUM(Positions, x)), FieldSpec::Num("y", F32, SOA_NUM(Positions, y)),
            FieldSpec::Num("z", F32, SOA_NUM(Positions, z)), FieldSpec::Num("w", F32, SOA_NUM(Positions, w)),
            FieldSpec::Num("a", F32, SOA_NUM(Positions, a)), FieldSpec::Num("b", F32, SOA_NUM(Positions, b)),
            FieldSpec::Num("c", F32, SOA_NUM(Positions, c)), FieldSpec::Num("d", F32, SOA_NUM(Positions, d)),
            FieldSpec::Num("e", F32, SOA_NUM(Positions, e)), FieldSpec::Num("f", F32, SOA_NUM(Positions, f)),
            FieldSpec::Num("g", F32, SOA_NUM(Positions, g)), FieldSpec::Num("h", F32, SOA_NUM(Positions, h)),
            FieldSpec::Num("i", F32, SOA_NUM(Positions, i)), FieldSpec::Num("j", F32, SOA_NUM(Positions, j)),
            FieldSpec::Num("k", F32, SOA_NUM(Positions, k)), FieldSpec::Num("l", F32, SOA_NUM(Positions, l)),
        } });

    // ---- Model: имя ассета (оно же рантайм-ссылка — фиксапа после загрузки нет) ----
    reg.Register({ .name = "Model", .sig_type = typeid(ModelComponent),
        .add_default = AddDefaultAoS<ModelComponent>,
        .fields = { FieldSpec::Str("name", AOS_STR(ModelComponent, name), AssetModel).ReadOnly() } });
        // ReadOnly: смена модели меняет состав батчей (и число сабмешей → длину списка
        // материалов), поэтому это будущая команда, а не запись строки из UI-потока

    // ---- Material: зубчатый массив имён — рукописная пара (см. выше) ----
    reg.Register({ .name = "Material", .sig_type = typeid(MaterialComponent),
        .add_default = AddDefaultAoS<MaterialComponent>,
        .custom_save = SaveMaterial, .custom_load = LoadMaterial });

    // ---- Draw: видимость + per-instance alpha/flags ----
    reg.Register({ .name = "Draw", .sig_type = typeid(DrawComponent),
        .add_default = AddDefaultAoS<DrawComponent>,
        .fields = {
            // visible менять ТОЛЬКО через EngineContext::HideEntity (дельта в батчи): generic-рендерер
            // его не рисует, правильный чекбокс-через-команду даёт ComponentExtraUI инспектора.
            FieldSpec::Num("visible", Bool, AOS_NUM(DrawComponent, visible)).Hidden(),
            FieldSpec::Num("alpha",   F32,  AOS_NUM(DrawComponent, alpha), 0, 1, 0.01f),
            FieldSpec::Num("flags",   U32,  AOS_NUM(DrawComponent, flags)),
        } });

    // ---- Теги (без данных): только дефолтный ряд ----
    reg.Register({ .name = "Shadow", .sig_type = typeid(ShadowComponent),
        .add_default = AddDefaultAoS<ShadowComponent> });

    // ---- LocalMatrix: SoA LocalMatrices, 16 колонок m0..m15 (column-major glm) ----
    reg.Register({ .name = "LocalMatrix", .sig_type = typeid(LocalMatrices),
        .add_default = AddDefaultSoA<LocalMatrices, LocalMatrixProxy16>,
        .fields = {
            FieldSpec::Num("m0",  F32, SOA_NUM(LocalMatrices, m0)),  FieldSpec::Num("m1",  F32, SOA_NUM(LocalMatrices, m1)),
            FieldSpec::Num("m2",  F32, SOA_NUM(LocalMatrices, m2)),  FieldSpec::Num("m3",  F32, SOA_NUM(LocalMatrices, m3)),
            FieldSpec::Num("m4",  F32, SOA_NUM(LocalMatrices, m4)),  FieldSpec::Num("m5",  F32, SOA_NUM(LocalMatrices, m5)),
            FieldSpec::Num("m6",  F32, SOA_NUM(LocalMatrices, m6)),  FieldSpec::Num("m7",  F32, SOA_NUM(LocalMatrices, m7)),
            FieldSpec::Num("m8",  F32, SOA_NUM(LocalMatrices, m8)),  FieldSpec::Num("m9",  F32, SOA_NUM(LocalMatrices, m9)),
            FieldSpec::Num("m10", F32, SOA_NUM(LocalMatrices, m10)), FieldSpec::Num("m11", F32, SOA_NUM(LocalMatrices, m11)),
            FieldSpec::Num("m12", F32, SOA_NUM(LocalMatrices, m12)), FieldSpec::Num("m13", F32, SOA_NUM(LocalMatrices, m13)),
            FieldSpec::Num("m14", F32, SOA_NUM(LocalMatrices, m14)), FieldSpec::Num("m15", F32, SOA_NUM(LocalMatrices, m15)),
        } });

    // ---- Parent: в файле СЫРОЙ файл-локальный id; ремап old→new — ObjectManager::LoadScene, проход 2 ----
    reg.Register({ .name = "Parent", .sig_type = typeid(ParentComponent),
        .add_default = AddDefaultAoS<ParentComponent>,
        .fields = { FieldSpec::Num("parent", U32, AOS_NUM(ParentComponent, parent)).ReadOnly() } });
        // ReadOnly: прямая запись parent рвёт обратный индекс scene->children

    reg.Register({ .name = "ShadowCaster", .sig_type = typeid(ShadowCasterComponent),
        .add_default = AddDefaultAoS<ShadowCasterComponent> });

    // ---- Лайты: только входные поля; приватные кэши пересчитаются (needsUpdate=true у T{}
    //      на загрузке и after_edit на правке в UI) ----
    reg.Register({ .name = "SpotLight", .sig_type = typeid(SpotLightComponent),
        .add_default = AddDefaultAoS<SpotLightComponent>,
        .fields = {
            FieldSpec::Num("source_radius", F32, AOS_NUM(SpotLightComponent, light_data.source_radius), 0, FLT_MAX, 0.01f),
            FieldSpec::Num("dir_x", F32, AOS_NUM(SpotLightComponent, light_data.dir_x), -1, 1, 0.01f),
            FieldSpec::Num("dir_y", F32, AOS_NUM(SpotLightComponent, light_data.dir_y), -1, 1, 0.01f),
            FieldSpec::Num("dir_z", F32, AOS_NUM(SpotLightComponent, light_data.dir_z), -1, 1, 0.01f),
            FieldSpec::Num("source_angle", Angle, AOS_NUM(SpotLightComponent, light_data.source_angle), 1, 89),
            FieldSpec::Num("r", F32, AOS_NUM(SpotLightComponent, light_data.r), 0, 1, 0.01f),
            FieldSpec::Num("g", F32, AOS_NUM(SpotLightComponent, light_data.g), 0, 1, 0.01f),
            FieldSpec::Num("b", F32, AOS_NUM(SpotLightComponent, light_data.b), 0, 1, 0.01f),
            FieldSpec::Num("power", F32, AOS_NUM(SpotLightComponent, light_data.power), 0, FLT_MAX),
            FieldSpec::Num("attenuation", F32, AOS_NUM(SpotLightComponent, light_data.attenuation), 0, FLT_MAX),
        },
        .after_edit = [](Archetype& a, size_t i) { (*a.get_array<SpotLightComponent>())[i].needsUpdate = true; } });

    reg.Register({ .name = "SphereLight", .sig_type = typeid(SphereLightComponent),
        .add_default = AddDefaultAoS<SphereLightComponent>,
        .fields = {
            FieldSpec::Num("source_radius", F32, AOS_NUM(SphereLightComponent, light_data.source_radius), 0, FLT_MAX, 0.01f),
            FieldSpec::Num("r", F32, AOS_NUM(SphereLightComponent, light_data.r), 0, 1, 0.01f),
            FieldSpec::Num("g", F32, AOS_NUM(SphereLightComponent, light_data.g), 0, 1, 0.01f),
            FieldSpec::Num("b", F32, AOS_NUM(SphereLightComponent, light_data.b), 0, 1, 0.01f),
            FieldSpec::Num("power", F32, AOS_NUM(SphereLightComponent, light_data.power), 0, FLT_MAX),
            FieldSpec::Num("attenuation", F32, AOS_NUM(SphereLightComponent, light_data.attenuation), 0, FLT_MAX),
        },
        .after_edit = [](Archetype& a, size_t i) { (*a.get_array<SphereLightComponent>())[i].needsUpdate = true; } });

    reg.Register({ .name = "DirectLight", .sig_type = typeid(DirectLightComponent),
        .add_default = AddDefaultAoS<DirectLightComponent>,
        .fields = {
            FieldSpec::Num("dir_x", F32, AOS_NUM(DirectLightComponent, light_data.dir_x), -1, 1, 0.01f),
            FieldSpec::Num("dir_y", F32, AOS_NUM(DirectLightComponent, light_data.dir_y), -1, 1, 0.01f),
            FieldSpec::Num("dir_z", F32, AOS_NUM(DirectLightComponent, light_data.dir_z), -1, 1, 0.01f),
            FieldSpec::Num("r", F32, AOS_NUM(DirectLightComponent, light_data.r), 0, 1, 0.01f),
            FieldSpec::Num("g", F32, AOS_NUM(DirectLightComponent, light_data.g), 0, 1, 0.01f),
            FieldSpec::Num("b", F32, AOS_NUM(DirectLightComponent, light_data.b), 0, 1, 0.01f),
            FieldSpec::Num("power", F32, AOS_NUM(DirectLightComponent, light_data.power), 0, FLT_MAX),
            FieldSpec::Num("center_x", F32, AOS_NUM(DirectLightComponent, light_data.center_x)),
            FieldSpec::Num("center_y", F32, AOS_NUM(DirectLightComponent, light_data.center_y)),
            FieldSpec::Num("center_z", F32, AOS_NUM(DirectLightComponent, light_data.center_z)),
            FieldSpec::Num("half_extent", F32, AOS_NUM(DirectLightComponent, light_data.half_extent), 0.01f, FLT_MAX, 0.1f),
            FieldSpec::Num("half_depth",  F32, AOS_NUM(DirectLightComponent, light_data.half_depth),  0.01f, FLT_MAX, 0.1f),
            // Кламп числа каскадов — ЕДИНОЖДЫ здесь (раньше дублировался в load и в инспекторе).
            FieldSpec::Num("cascade_count", U32, AOS_NUM(DirectLightComponent, light_data.cascade_count),
                           1, (float)DirectLightComponent::DirectLightData::MAX_CASCADES, 1).Clamp(),
            FieldSpec::Num("cascade_ratio", F32, AOS_NUM(DirectLightComponent, light_data.cascade_ratio), 1, FLT_MAX),
        },
        .after_edit = [](Archetype& a, size_t i) { (*a.get_array<DirectLightComponent>())[i].needsUpdate = true; } });

    // ---- Mass ----
    reg.Register({ .name = "Mass", .sig_type = typeid(MassComponent),
        .add_default = AddDefaultAoS<MassComponent>,
        .fields = { FieldSpec::Num("mass", F32, AOS_NUM(MassComponent, mass), 0, FLT_MAX) } });

    // ---- EditorHidden (тег: скрыт из списка UI; персистится как авторское состояние) ----
    reg.Register({ .name = "EditorHidden", .sig_type = typeid(EditorHiddenComponent),
        .add_default = AddDefaultAoS<EditorHiddenComponent> });

    // ---- Velocity / Acceleration (SoA x,y,z) ----
    reg.Register({ .name = "Velocity", .sig_type = typeid(Velocities),
        .add_default = AddDefaultSoA<Velocities, VelocityProxy>,
        .fields = {
            FieldSpec::Num("x", F32, SOA_NUM(Velocities, x)),
            FieldSpec::Num("y", F32, SOA_NUM(Velocities, y)),
            FieldSpec::Num("z", F32, SOA_NUM(Velocities, z)),
        } });

    reg.Register({ .name = "Acceleration", .sig_type = typeid(Accelerations),
        .add_default = AddDefaultSoA<Accelerations, AccelerationProxy>,
        .fields = {
            FieldSpec::Num("x", F32, SOA_NUM(Accelerations, x)),
            FieldSpec::Num("y", F32, SOA_NUM(Accelerations, y)),
            FieldSpec::Num("z", F32, SOA_NUM(Accelerations, z)),
        } });
}

#undef AOS_NUM
#undef SOA_NUM
#undef AOS_STR
