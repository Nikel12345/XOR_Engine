#include "PCH.h"
#include "MaterialParamsSpec.h"
#include "MaterialParams.h"   // Opaque/TransparentMaterialParams — встроенные раскладки

// ============================================================
//  Реестр (зеркало ComponentSpecRegistry)
// ============================================================
MaterialParamsSpecRegistry& MaterialParamsSpecRegistry::Get()
{
    static MaterialParamsSpecRegistry instance;
    return instance;
}

void MaterialParamsSpecRegistry::Register(MaterialParamsSpec s)
{
    if (s.name.empty()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "MaterialParamsSpec: empty name - not registered");
        return;
    }
    if (by_name_.count(s.name)) return;   // идемпотентно: уже зарегали

    // Схема обязана лежать внутри блоба: поле за границей sizeof(T) — ложь о раскладке,
    // с ней UI писал бы мимо структуры. Отбрасываем поимённо, тип регистрируем без него.
    for (size_t i = 0; i < s.fields.size(); ) {
        const MatFieldSpec& f = s.fields[i];
        if (f.key && f.offset + f.Bytes() <= s.size) { ++i; continue; }
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "MaterialParamsSpec '%s': field '%s' (offset %u, %u bytes) does not fit into %zu-byte blob - dropped",
            s.name.c_str(), f.key ? f.key : "<null>", f.offset, f.Bytes(), s.size);
        s.fields.erase(s.fields.begin() + i);
    }

    const size_t idx = specs_.size();
    by_name_[s.name] = idx;
    by_type_[s.type] = idx;
    specs_.push_back(std::move(s));
}

const MaterialParamsSpec* MaterialParamsSpecRegistry::ByName(const std::string& name) const
{
    auto it = by_name_.find(name);
    return it == by_name_.end() ? nullptr : &specs_[it->second];
}

const MaterialParamsSpec* MaterialParamsSpecRegistry::ByType(std::type_index t) const
{
    auto it = by_type_.find(t);
    return it == by_type_.end() ? nullptr : &specs_[it->second];
}

const std::string& MaterialParamsTypeName(std::type_index t)
{
    static const std::string kNone;
    if (const MaterialParamsSpec* s = MaterialParamsSpecRegistry::Get().ByType(t)) return s->name;
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
        "SetMaterialParams: type '%s' is not registered (MaterialParamsSpecRegistry::Register) - "
        "the blob reaches the shader, but the inspector cannot edit it and SaveScene will drop it",
        t.name());
    return kNone;
}

void ApplyMaterialParamsSpec(Material* m, const MaterialParamsSpec& s)
{
    if (!m) return;
    m->params      = s.defaults;   // дефолты = member-инициализаторы структуры типа
    m->params_type = s.name;
}

void ClearMaterialParams(Material* m)
{
    if (!m) return;
    m->params.clear();
    m->params_type.clear();
}

// ============================================================
//  Встроенные типы: вся правда о типе — одна запись (как у компонентов).
//  Порядок fields = порядок ключей в materials.json и полей в инспекторе.
// ============================================================
void RegisterBuiltinMaterialParamsSpecs()
{
    using K = MatFieldKind;
    auto& reg = MaterialParamsSpecRegistry::Get();

    // ---- Opaque: тинт + эмиссия + Blinn-Phong + POM (см. комментарии в MaterialParams.h) ----
    reg.Register(MakeMaterialParamsSpec<OpaqueMaterialParams>("Opaque", {
        MatFieldSpec::Num(MAT_FIELD(OpaqueMaterialParams, baseColor),        K::Color4).Label("Base Color"),
        MatFieldSpec::Num(MAT_FIELD(OpaqueMaterialParams, emissive),         K::Color3).Label("Emissive"),
        MatFieldSpec::Num(MAT_FIELD(OpaqueMaterialParams, emissiveStrength), K::F32, 0, 8).Label("Emissive Strength"),
        MatFieldSpec::Num(MAT_FIELD(OpaqueMaterialParams, metallic),         K::F32, 0, 1).Label("Metallic"),
        MatFieldSpec::Num(MAT_FIELD(OpaqueMaterialParams, roughness),        K::F32, 0, 1).Label("Roughness"),
        // heightScale: 0 = POM выключен; рабочая зона ≤0.05 (глубже — «плавание» на близи).
        MatFieldSpec::Num(MAT_FIELD(OpaqueMaterialParams, heightScale),      K::F32, 0, 0.2f, 0.001f).Label("POM Height"),
        // pomBias: АБСОЛЮТНЫЙ пол LOD рельефа, 1..3 (не выключатель); требует мипов у normal-атласа.
        MatFieldSpec::Num(MAT_FIELD(OpaqueMaterialParams, pomBias),          K::F32, 0, 3).Label("POM Bias"),
    }));

    // ---- Transparent: одна альфа (остальное — padding до 16 байт) ----
    reg.Register(MakeMaterialParamsSpec<TransparentMaterialParams>("Transparent", {
        MatFieldSpec::Num(MAT_FIELD(TransparentMaterialParams, alpha), K::F32, 0, 1).Label("Alpha"),
    }));

    // ---- UI: тинт фона + цвет текста (см. MaterialParams.h) ----
    reg.Register(MakeMaterialParamsSpec<UIMaterialParams>("UI", {
        MatFieldSpec::Num(MAT_FIELD(UIMaterialParams, bg_color),    K::Color4).Label("BG Color"),
        MatFieldSpec::Num(MAT_FIELD(UIMaterialParams, text_color),  K::Color4).Label("Text Color"),
        MatFieldSpec::Num(MAT_FIELD(UIMaterialParams, text_height), K::F32, 0, 1).Label("Text Height"),
        MatFieldSpec::Num(MAT_FIELD(UIMaterialParams, text_anchor), K::F32, 0, 1).Label("Text Anchor (0=top,1=bot)"),
    }));
}
