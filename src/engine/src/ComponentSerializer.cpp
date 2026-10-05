#include "PCH.h"
#include "ComponentSerializer.h"
#include "BaseComponents.h"
#include <algorithm>
#include <bit>
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
    if (by_name_.count(s.name)) return;
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

FieldSpec FieldSpec::Num(const char* key, FieldKind kind,
                         double (*get)(Archetype&, size_t), void (*set)(Archetype&, size_t, double),
                         FieldColumn (*column)(Archetype&),
                         float lo, float hi, float speed)
{
    FieldSpec f;
    f.key = key; f.kind = kind;
    f.get_num = get; f.set_num = set; f.column = column;
    f.lo = lo; f.hi = hi; f.speed = speed;
    return f;
}

FieldSpec FieldSpec::Str(const char* key,
                         std::function<const std::string&(Archetype&, size_t)> get,
                         std::function<void(Archetype&, size_t, std::string)> set,
                         FieldKind kind)
{
    FieldSpec f;
    f.key = key; f.kind = kind;
    f.get_str = std::move(get); f.set_str = std::move(set);
    return f;
}

void ComponentSpec::Save(Archetype& arch, size_t count, sheaf::Writer& w, std::vector<sheaf::Column>& out) const
{
    if (custom_save) { custom_save(arch, count, w, out); return; }
    if (fields.empty()) return;

    // Дефолт поля уходит в файл, и берётся он из того же T{}/прокси, что и у рантайм-создания.
    Archetype def_row;
    add_default(def_row);

    for (const FieldSpec& f : fields) {
        if (!f.set_num && !f.set_str) continue;
        sheaf::Column& c = out.emplace_back();
        c.name = f.key;
        c.values.reserve(count);
        switch (f.kind) {
        case FieldKind::F32:
        case FieldKind::Angle:
            c.type = sheaf::Type::F32;
            c.def  = sheaf::Bits(safe_d_f(f.get_num(def_row, 0)));
            for (size_t i = 0; i < count; ++i) c.values.push_back(sheaf::Bits(safe_d_f(f.get_num(arch, i))));
            break;
        case FieldKind::U32:
            c.type = sheaf::Type::U32;
            c.def  = safe_d_u32(f.get_num(def_row, 0));
            for (size_t i = 0; i < count; ++i) c.values.push_back(safe_d_u32(f.get_num(arch, i)));
            break;
        case FieldKind::Bool:
            c.type = sheaf::Type::Bool;
            c.def  = f.get_num(def_row, 0) != 0.0;
            for (size_t i = 0; i < count; ++i) c.values.push_back(f.get_num(arch, i) != 0.0);
            break;
        default:
            c.type = sheaf::Type::Str;
            c.def  = w.Intern(f.get_str(def_row, 0));
            for (size_t i = 0; i < count; ++i) c.values.push_back(w.Intern(f.get_str(arch, i)));
            break;
        }
    }
}

namespace {

const sheaf::Column* FindColumn(const sheaf::Component& comp, const char* key)
{
    for (const sheaf::Column& c : comp.fields)
        if (c.name == key) return &c;
    return nullptr;
}

// Тип колонки в файле мог разойтись с полем после правки компонента: число читается из любого
// числового типа.
bool CellNum(const sheaf::Column& c, size_t i, double& out)
{
    if ((c.flags & sheaf::Nullable) && !c.present[i]) return false;
    const uint32_t v = c.values[i];
    switch (c.type) {
    case sheaf::Type::F32: out = std::bit_cast<float>(v);   return true;
    case sheaf::Type::I32: out = std::bit_cast<int32_t>(v); return true;
    case sheaf::Type::Str: return false;
    default:               out = v;                         return true;
    }
}

// Колонку файла можно положить в поле побитно: оба 4-байтные, и смысл битов один.
bool SameBits(sheaf::Type file, FieldColumn::Type field)
{
    switch (field) {
    case FieldColumn::Type::F32: return file == sheaf::Type::F32;
    case FieldColumn::Type::U32: return file == sheaf::Type::U32 || file == sheaf::Type::Ref;
    case FieldColumn::Type::I32: return file == sheaf::Type::I32;
    default:                     return false;
    }
}

} // namespace

void ComponentSpec::LoadDefaults(Archetype& arch, size_t count) const
{
    if (custom_load) { custom_load(arch, nullptr, count, {}); return; }
    if (count == 0) return;
    add_default(arch);                                        // заводит массив компонента
    arch.components.at(sig_type)->repeat_last(count - 1);
}

void ComponentSpec::BeginLoad(Archetype& arch, const sheaf::Component& header, size_t count, std::span<sheaf::Destination> dests) const
{
    if (custom_load || count == 0) return;
    LoadDefaults(arch, count);
    const size_t base = arch.entities.size() - count;
    for (size_t i = 0; i < header.fields.size(); ++i) {
        const sheaf::Column& c = header.fields[i];
        const auto f = std::find_if(fields.begin(), fields.end(), [&](const FieldSpec& x) { return c.name == x.key; });
        if (f == fields.end() || !f->column || f->clamp_on_load) continue;
        const FieldColumn col = f->column(arch);
        if (SameBits(c.type, col.type)) dests[i] = { col.first + base * col.stride, col.stride };
    }
}

void ComponentSpec::FinishLoad(Archetype& arch, const sheaf::Component& comp, size_t count, std::span<const std::string> strings) const
{
    if (custom_load) { custom_load(arch, &comp, count, strings); return; }
    const size_t base = arch.entities.size() - count;
    for (const FieldSpec& f : fields) {
        if (!f.set_num && !f.set_str) continue;
        const sheaf::Column* c = FindColumn(comp, f.key);
        if (!c || c->direct) continue;                        // нет колонки → остаётся дефолт
        if ((c->flags & sheaf::List) || (c->type == sheaf::Type::Str) != static_cast<bool>(f.set_str)) {
            SDL_Log("LoadScene: %s.%s in file does not fit the field type - defaults kept", name.c_str(), f.key);
            continue;
        }
        if (f.set_str) {
            for (size_t i = 0; i < count; ++i)
                if (!(c->flags & sheaf::Nullable) || c->present[i]) f.set_str(arch, base + i, strings[c->values[i]]);
            continue;
        }
        for (size_t i = 0; i < count; ++i) {
            double d;
            if (!CellNum(*c, i, d)) continue;
            if (f.clamp_on_load) d = d < f.lo ? f.lo : (d > f.hi ? f.hi : d);
            f.set_num(arch, base + i, d);
        }
    }
}

// Список имён НА СУЩНОСТЬ — зубчатый массив, а не колонка одного поля: схемой не выражается.
namespace {

// Число строк лежит в ДАННЫХ, поэтому одно вычисляемое строковое поле, а не MAX_CASCADES
// числовых, часть которых описывала бы несуществующие каскады.
std::string FormatCascades(const DirectLightComponent::DirectLightData& d)
{
    std::string out;
    char line[96];
    for (int c = 0; c < d.cascade_count; ++c) {
        const float he = d.CascadeExtent(c);
        const float dp = d.CascadeDepth(c);
        snprintf(line, sizeof(line), "c%d: %.1f x %.1f, depth %.1f, texel %.4f",
                 c, 2.0f * he, 2.0f * he, 2.0f * dp, (2.0f * he) / 1024.0f);
        if (c) out += '\n';
        out += line;
    }
    return out;
}

} // namespace

// Порядок fields — это порядок колонок в файле.
void RegisterBuiltinComponentSpecs()
{
    using enum FieldKind;
    auto& reg = ComponentSpecRegistry::Get();

    reg.Register({ .name = "Transform", .sig_type = typeid(Positions),
        .add_default = AddDefaultSoA<Positions, PositionProxy16>,
        .fields = {
            FieldSpec::Num("x", F32, SOA_NUM(Positions, x)).Group(FieldGroup::Mat4), FieldSpec::Num("y", F32, SOA_NUM(Positions, y)),
            FieldSpec::Num("z", F32, SOA_NUM(Positions, z)), FieldSpec::Num("w", F32, SOA_NUM(Positions, w)),
            FieldSpec::Num("a", F32, SOA_NUM(Positions, a)), FieldSpec::Num("b", F32, SOA_NUM(Positions, b)),
            FieldSpec::Num("c", F32, SOA_NUM(Positions, c)), FieldSpec::Num("d", F32, SOA_NUM(Positions, d)),
            FieldSpec::Num("e", F32, SOA_NUM(Positions, e)), FieldSpec::Num("f", F32, SOA_NUM(Positions, f)),
            FieldSpec::Num("g", F32, SOA_NUM(Positions, g)), FieldSpec::Num("h", F32, SOA_NUM(Positions, h)),
            FieldSpec::Num("i", F32, SOA_NUM(Positions, i)), FieldSpec::Num("j", F32, SOA_NUM(Positions, j)),
            FieldSpec::Num("k", F32, SOA_NUM(Positions, k)), FieldSpec::Num("l", F32, SOA_NUM(Positions, l)),
        } });

    reg.Register({ .name = "Shadow", .sig_type = typeid(ShadowComponent),
        .add_default = AddDefaultAoS<ShadowComponent> });

    reg.Register({ .name = "TextureState", .sig_type = typeid(TextureStateComponent),
        .add_default = AddDefaultAoS<TextureStateComponent> });

    reg.Register({ .name = "LocalMatrix", .sig_type = typeid(LocalMatrices),
        .add_default = AddDefaultSoA<LocalMatrices, LocalMatrixProxy16>,
        .fields = {
            FieldSpec::Num("m0",  F32, SOA_NUM(LocalMatrices, m0)).Group(FieldGroup::Mat4), FieldSpec::Num("m1",  F32, SOA_NUM(LocalMatrices, m1)),
            FieldSpec::Num("m2",  F32, SOA_NUM(LocalMatrices, m2)),  FieldSpec::Num("m3",  F32, SOA_NUM(LocalMatrices, m3)),
            FieldSpec::Num("m4",  F32, SOA_NUM(LocalMatrices, m4)),  FieldSpec::Num("m5",  F32, SOA_NUM(LocalMatrices, m5)),
            FieldSpec::Num("m6",  F32, SOA_NUM(LocalMatrices, m6)),  FieldSpec::Num("m7",  F32, SOA_NUM(LocalMatrices, m7)),
            FieldSpec::Num("m8",  F32, SOA_NUM(LocalMatrices, m8)),  FieldSpec::Num("m9",  F32, SOA_NUM(LocalMatrices, m9)),
            FieldSpec::Num("m10", F32, SOA_NUM(LocalMatrices, m10)), FieldSpec::Num("m11", F32, SOA_NUM(LocalMatrices, m11)),
            FieldSpec::Num("m12", F32, SOA_NUM(LocalMatrices, m12)), FieldSpec::Num("m13", F32, SOA_NUM(LocalMatrices, m13)),
            FieldSpec::Num("m14", F32, SOA_NUM(LocalMatrices, m14)), FieldSpec::Num("m15", F32, SOA_NUM(LocalMatrices, m15)),
        } });

    // В файле лежит сквозной номер объекта (ref), настоящим id его делает проход 2 ObjectManager::LoadScene.
    reg.Register({ .name = "Parent", .sig_type = typeid(ParentComponent),
        .add_default = AddDefaultAoS<ParentComponent>,
        .fields = { FieldSpec::Num("parent", U32, AOS_NUM(ParentComponent, parent)).ReadOnly() } });
        // ReadOnly: прямая запись parent порвала бы обратный индекс scene->children

    reg.Register({ .name = "ShadowCaster", .sig_type = typeid(ShadowCasterComponent),
        .add_default = AddDefaultAoS<ShadowCasterComponent> });

    // Объявлены только входные поля: кэши пересчитает сам компонент по needsUpdate, который
    // приезжает из T{} на загрузке и из after_edit на правке в UI.
    reg.Register({ .name = "SpotLight", .sig_type = typeid(SpotLightComponent),
        .add_default = AddDefaultAoS<SpotLightComponent>,
        .fields = {
            FieldSpec::Num("source_radius", F32, AOS_NUM(SpotLightComponent, light_data.source_radius), 0, FLT_MAX, 0.01f),
            FieldSpec::Num("dir_x", F32, AOS_NUM(SpotLightComponent, light_data.dir_x), -1, 1, 0.01f).Group(FieldGroup::Vec3, "dir"),
            FieldSpec::Num("dir_y", F32, AOS_NUM(SpotLightComponent, light_data.dir_y), -1, 1, 0.01f),
            FieldSpec::Num("dir_z", F32, AOS_NUM(SpotLightComponent, light_data.dir_z), -1, 1, 0.01f),
            FieldSpec::Num("source_angle", Angle, AOS_NUM(SpotLightComponent, light_data.source_angle), 1, 89),
            FieldSpec::Num("r", F32, AOS_NUM(SpotLightComponent, light_data.r), 0, 1, 0.01f).Group(FieldGroup::Color3, "color"),
            FieldSpec::Num("g", F32, AOS_NUM(SpotLightComponent, light_data.g), 0, 1, 0.01f),
            FieldSpec::Num("b", F32, AOS_NUM(SpotLightComponent, light_data.b), 0, 1, 0.01f),
            FieldSpec::Num("power", F32, AOS_NUM(SpotLightComponent, light_data.power), 0, FLT_MAX),
            FieldSpec::Num("attenuation", F32, AOS_NUM(SpotLightComponent, light_data.attenuation), 0, FLT_MAX),
            FieldSpec::Num("max_distance", F32,
                AOS_CALC(SpotLightComponent, (c.light_data.ResolveDistance(), c.light_data.GetMaxDistance()))),
        },
        .after_edit = [](Archetype& a, size_t i) { (*a.get_array<SpotLightComponent>())[i].needsUpdate = true; } });

    reg.Register({ .name = "SphereLight", .sig_type = typeid(SphereLightComponent),
        .add_default = AddDefaultAoS<SphereLightComponent>,
        .fields = {
            FieldSpec::Num("source_radius", F32, AOS_NUM(SphereLightComponent, light_data.source_radius), 0, FLT_MAX, 0.01f),
            FieldSpec::Num("r", F32, AOS_NUM(SphereLightComponent, light_data.r), 0, 1, 0.01f).Group(FieldGroup::Color3, "color"),
            FieldSpec::Num("g", F32, AOS_NUM(SphereLightComponent, light_data.g), 0, 1, 0.01f),
            FieldSpec::Num("b", F32, AOS_NUM(SphereLightComponent, light_data.b), 0, 1, 0.01f),
            FieldSpec::Num("power", F32, AOS_NUM(SphereLightComponent, light_data.power), 0, FLT_MAX),
            FieldSpec::Num("attenuation", F32, AOS_NUM(SphereLightComponent, light_data.attenuation), 0, FLT_MAX),
            FieldSpec::Num("max_distance", F32,
                AOS_CALC(SphereLightComponent, (c.light_data.ResolveDistance(), c.light_data.GetMaxDistance()))),
        },
        .after_edit = [](Archetype& a, size_t i) { (*a.get_array<SphereLightComponent>())[i].needsUpdate = true; } });

    reg.Register({ .name = "DirectLight", .sig_type = typeid(DirectLightComponent),
        .add_default = AddDefaultAoS<DirectLightComponent>,
        .fields = {
            FieldSpec::Num("dir_x", F32, AOS_NUM(DirectLightComponent, light_data.dir_x), -1, 1, 0.01f).Group(FieldGroup::Vec3, "dir"),
            FieldSpec::Num("dir_y", F32, AOS_NUM(DirectLightComponent, light_data.dir_y), -1, 1, 0.01f),
            FieldSpec::Num("dir_z", F32, AOS_NUM(DirectLightComponent, light_data.dir_z), -1, 1, 0.01f),
            FieldSpec::Num("r", F32, AOS_NUM(DirectLightComponent, light_data.r), 0, 1, 0.01f).Group(FieldGroup::Color3, "color"),
            FieldSpec::Num("g", F32, AOS_NUM(DirectLightComponent, light_data.g), 0, 1, 0.01f),
            FieldSpec::Num("b", F32, AOS_NUM(DirectLightComponent, light_data.b), 0, 1, 0.01f),
            FieldSpec::Num("power", F32, AOS_NUM(DirectLightComponent, light_data.power), 0, FLT_MAX),
            FieldSpec::Num("center_x", F32, AOS_NUM(DirectLightComponent, light_data.center_x)).Group(FieldGroup::Vec3, "center"),
            FieldSpec::Num("center_y", F32, AOS_NUM(DirectLightComponent, light_data.center_y)),
            FieldSpec::Num("center_z", F32, AOS_NUM(DirectLightComponent, light_data.center_z)),
            FieldSpec::Num("half_extent", F32, AOS_NUM(DirectLightComponent, light_data.half_extent), 0.01f, FLT_MAX, 0.1f),
            FieldSpec::Num("half_depth",  F32, AOS_NUM(DirectLightComponent, light_data.half_depth),  0.01f, FLT_MAX, 0.1f),
            FieldSpec::Num("cascade_count", U32, AOS_NUM(DirectLightComponent, light_data.cascade_count),
                           1, (float)DirectLightComponent::DirectLightData::MAX_CASCADES, 1).Clamp(),
            FieldSpec::Num("cascade_ratio", F32, AOS_NUM(DirectLightComponent, light_data.cascade_ratio), 1, FLT_MAX),
            FieldSpec::Str("cascades", AOS_CALC_STR(DirectLightComponent, FormatCascades(c.light_data))),
        },
        .after_edit = [](Archetype& a, size_t i) { (*a.get_array<DirectLightComponent>())[i].needsUpdate = true; } });

    reg.Register({ .name = "EditorHidden", .sig_type = typeid(EditorHiddenComponent),
        .add_default = AddDefaultAoS<EditorHiddenComponent> });

    reg.Register({ .name = "Velocity", .sig_type = typeid(Velocities),
        .add_default = AddDefaultSoA<Velocities, VelocityProxy>,
        .fields = {
            FieldSpec::Num("x", F32, SOA_NUM(Velocities, x)).Group(FieldGroup::Vec3, "velocity"),
            FieldSpec::Num("y", F32, SOA_NUM(Velocities, y)),
            FieldSpec::Num("z", F32, SOA_NUM(Velocities, z)),
        } });

    reg.Register({ .name = "Acceleration", .sig_type = typeid(Accelerations),
        .add_default = AddDefaultSoA<Accelerations, AccelerationProxy>,
        .fields = {
            FieldSpec::Num("x", F32, SOA_NUM(Accelerations, x)).Group(FieldGroup::Vec3, "acceleration"),
            FieldSpec::Num("y", F32, SOA_NUM(Accelerations, y)),
            FieldSpec::Num("z", F32, SOA_NUM(Accelerations, z)),
        } });
}

#undef AOS_NUM
#undef SOA_NUM
#undef AOS_STR
