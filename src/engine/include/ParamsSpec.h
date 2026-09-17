#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <typeindex>
#include <functional>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <SDL3/SDL_log.h>
#include "MaterialData.h"

inline constexpr size_t kMaxParamsBlob = 128;

// Лейн — 4 байта, как в cbuffer (bool в HLSL тоже 4). Angle держит радианы и в блобе, и в файле,
// градусы живут только в слайдере, поэтому lo/hi у него задают в ГРАДУСАХ.
enum class ParamsFieldKind : uint8_t { F32, Angle, U32, Bool, Vec2, Vec3, Vec4, Color3, Color4 };

inline uint32_t ParamsFieldLanes(ParamsFieldKind k)
{
    switch (k) {
    case ParamsFieldKind::Vec2:                        return 2;
    case ParamsFieldKind::Vec3: case ParamsFieldKind::Color3: return 3;
    case ParamsFieldKind::Vec4: case ParamsFieldKind::Color4: return 4;
    default:                                        return 1;
    }
}
inline uint32_t ParamsFieldBytes(ParamsFieldKind k) { return 4u * ParamsFieldLanes(k); }

struct ParamsFieldSpec {
    const char*  key    = nullptr;   // json-ключ поля и, при пустом label, подпись в UI
    const char*  label  = nullptr;
    ParamsFieldKind kind   = ParamsFieldKind::F32;
    uint32_t     offset = 0;

    float lo = 0, hi = 0;            // lo == hi → диапазон не задан
    float speed = 0.01f;
    bool  clamp_on_load = false;
    bool  ui_readonly   = false;

    static ParamsFieldSpec Num(const char* key, uint32_t offset, ParamsFieldKind kind,
                            float lo = 0, float hi = 0, float speed = 0.01f)
    {
        ParamsFieldSpec f;
        f.key = key; f.offset = offset; f.kind = kind;
        f.lo = lo; f.hi = hi; f.speed = speed;
        return f;
    }

    ParamsFieldSpec&& Label(const char* l) && { label = l;            return std::move(*this); }
    ParamsFieldSpec&& Clamp()              && { clamp_on_load = true; return std::move(*this); }
    ParamsFieldSpec&& ReadOnly()           && { ui_readonly   = true; return std::move(*this); }

    const char* UiLabel() const { return label ? label : key; }
    uint32_t    Bytes()   const { return ParamsFieldBytes(kind); }
};

#define PARAMS_FIELD(T, member) #member, (uint32_t)offsetof(T, member)

struct ParamsSpec {
    std::string     name; 
    std::type_index type = std::type_index(typeid(void));
    size_t          size = 0;
    std::vector<uint8_t>      defaults;
    std::vector<ParamsFieldSpec> fields;
    // Задан — инспектор зовёт его вместо generic-рендерера.
    std::function<void(void* blob)> custom_edit;

    const ParamsFieldSpec* Field(const char* key) const {
        for (const ParamsFieldSpec& f : fields) if (f.key && key && std::strcmp(f.key, key) == 0) return &f;
        return nullptr;
    }
};

class ParamsSpecRegistry {
public:
    static ParamsSpecRegistry& Materials();
    static ParamsSpecRegistry& Passes();

    // Идемпотентно по имени.
    void Register(ParamsSpec s);

    const ParamsSpec* ByName(const std::string& name) const;
    const ParamsSpec* ByType(std::type_index t) const;
    const std::vector<ParamsSpec>& All() const { return specs_; }

private:
    std::vector<ParamsSpec>             specs_;
    std::unordered_map<std::string, size_t>     by_name_;
    std::unordered_map<std::type_index, size_t> by_type_;
};

template<class T>
ParamsSpec MakeParamsSpec(std::string name, std::vector<ParamsFieldSpec> fields)
{
    static_assert(std::is_trivially_copyable_v<T>, "params-блоб копируется байтами в cbuffer");
    static_assert(sizeof(T) % 16 == 0, "раскладка cbuffer обязана быть кратна 16 байтам");
    static_assert(sizeof(T) <= kMaxParamsBlob, "params-блоб не влезает в cbuffer (kMaxParamsBlob)");
    ParamsSpec s;
    s.name = std::move(name);
    s.type = std::type_index(typeid(T));
    s.size = sizeof(T);
    const T d{};
    s.defaults.resize(sizeof(T));
    std::memcpy(s.defaults.data(), &d, sizeof(T));
    s.fields = std::move(fields);
    return s;
}

// Незарегистрированный тип даёт "" и ошибку в лог: блоб уедет в рендер, но UI его не разберёт,
// а SaveScene не сохранит.
const std::string& MaterialParamsTypeName(std::type_index t);

// nullptr, если поле не влезает в блоб — схема разошлась с раскладкой.
inline void* ParamsFieldPtr(std::vector<uint8_t>& blob, const ParamsFieldSpec& f)
{
    return (f.offset + f.Bytes() <= blob.size()) ? static_cast<void*>(blob.data() + f.offset) : nullptr;
}
inline const void* ParamsFieldPtr(const std::vector<uint8_t>& blob, const ParamsFieldSpec& f)
{
    return (f.offset + f.Bytes() <= blob.size()) ? static_cast<const void*>(blob.data() + f.offset) : nullptr;
}

// Адресат — именно эта sp материала: нет её у материала, значит блобу некому ехать, и это ошибка.
void SetMaterialParamsBlob(Material* m, ShaderProgramId sp_id, const std::string& sp_name,
                           const void* data, size_t size, const std::string& type_name);

// T обязан совпадать с раскладкой cbuffer MaterialBlock этой sp.
template<class T>
void SetMaterialParams(Material* m, ShaderProgramId sp_id, const std::string& sp_name, const T& p)
{
    SetMaterialParamsBlob(m, sp_id, sp_name, &p, sizeof(T), MaterialParamsTypeName(std::type_index(typeid(T))));
}

void ApplyMaterialParamsSpec(SpBinding* b, const ParamsSpec& s);
void ClearMaterialParams(SpBinding* b);

template<class P, class T>
void SetPassState(P* step, const T& v)
{
    static_assert(std::is_trivially_copyable_v<T>, "состояние прохода копируется байтами");
    if (!step) return;
    step->state.resize(sizeof(T));
    std::memcpy(step->state.data(), &v, sizeof(T));
    step->state_type.clear();
}

template<class P, class T>
void SetPassState(P* step, const std::string& spec_name, const T& v)
{
    static_assert(std::is_trivially_copyable_v<T>, "состояние прохода копируется байтами");
    if (!step) return;
    const ParamsSpec* s = ParamsSpecRegistry::Passes().ByName(spec_name);
    if (!s || s->size != sizeof(T)) {
        // Блоб всё равно ставим: тело прохода и push-функции обязаны получить свои байты.
        // Не сойдётся только редактор.
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "SetPassState: spec '%s' is not registered or its size differs from sizeof(T) - "
            "the state still reaches the shaders, but the inspector cannot edit it",
            spec_name.c_str());
    }
    step->state.resize(sizeof(T));
    std::memcpy(step->state.data(), &v, sizeof(T));
    step->state_type = spec_name;
}

void RegisterBuiltinMaterialParamsSpecs();
