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
        "%.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g",
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
    std::snprintf(b, sizeof b, "%d %.9g %u",
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

} // namespace

void RegisterBuiltinComponentSerializers()
{
    auto& reg = ComponentSerializerRegistry::Get();
    reg.Register({ "Transform", std::type_index(typeid(Positions)),         &SaveTransform, &LoadTransform });
    reg.Register({ "Model",     std::type_index(typeid(ModelComponent)),    &SaveModel,     &LoadModel });
    reg.Register({ "Material",  std::type_index(typeid(MaterialComponent)), &SaveMaterial,  &LoadMaterial });
    reg.Register({ "Draw",      std::type_index(typeid(DrawComponent)),     &SaveDraw,      &LoadDraw });
    reg.Register({ "Shadow",    std::type_index(typeid(ShadowComponent)),   &SaveShadow,    &LoadShadow });
}
