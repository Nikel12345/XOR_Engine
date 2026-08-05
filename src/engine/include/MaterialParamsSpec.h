#pragma once
// Реестр ТИПОВ параметров материала — прямое зеркало ComponentSerializer.h, но для блоба
// Material::params. Тип params ДЕКЛАРИРУЕТ схему полей (fields), а редактор в инспекторе и
// запись/чтение materials.json — генерируемые интерпретаторы этой схемы: поле объявляется
// ОДИН РАЗ, диапазоны и дефолты не расходятся между файлом и UI.
//
// Регистрация ОТКРЫТАЯ и полностью симметрична компонентам: движок регистрирует свои типы в
// RegisterBuiltinMaterialParamsSpecs(), верхние слои (игра) — из своего кода одной записью,
// НЕ ПРАВЯ НИ ОДНОГО ФАЙЛА ДВИЖКА (раньше это было невозможно: тип-тег жил закрытым enum'ом
// MaterialParamsKind внутри движка, и его строковый round-trip — рукописным switch в Engine_Scene):
//
//   struct alignas(16) WaterParams { float tint[4]{0.2f,0.5f,0.9f,1}; float waveAmp = 0.1f, waveSpeed = 1, foam = 0, _pad = 0; };
//   MaterialParamsSpecRegistry::Get().Register(MakeMaterialParamsSpec<WaterParams>("Water", {
//       MatFieldSpec::Num(MAT_FIELD(WaterParams, tint),      MatFieldKind::Color4).Label("Tint"),
//       MatFieldSpec::Num(MAT_FIELD(WaterParams, waveAmp),   MatFieldKind::F32, 0, 1),
//       MatFieldSpec::Num(MAT_FIELD(WaterParams, waveSpeed), MatFieldKind::F32, 0, 4),
//   }));
//
// После этого тип сам появляется в дропдауне «Type» инспектора, его поля рисует generic-рендерер
// (ui::DrawMaterialParamsFields), а материалы с ним сохраняются/грузятся по ИМЕНАМ полей.
//
// Зависимостей нет намеренно: ни ImGui (рисует UI-слой), ни yyjson (пишет Engine_Scene) — чтобы
// заголовок был подключаем из любого пользовательского кода.
#include <string>
#include <vector>
#include <unordered_map>
#include <typeindex>
#include <functional>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include "MaterialData.h"

// Вид поля: сколько 4-байтовых лейнов занимает в блобе и каким виджетом рисуется.
// Все лейны по 4 байта — как в cbuffer (bool в HLSL тоже 4 байта, на CPU держим uint32_t).
// Angle — радианы в блобе и в файле (как F32), слайдер UI в градусах; lo/hi у него — ГРАДУСЫ.
enum class MatFieldKind : uint8_t { F32, Angle, U32, Bool, Vec2, Vec3, Vec4, Color3, Color4 };

inline uint32_t MatFieldLanes(MatFieldKind k)
{
    switch (k) {
    case MatFieldKind::Vec2:                        return 2;
    case MatFieldKind::Vec3: case MatFieldKind::Color3: return 3;
    case MatFieldKind::Vec4: case MatFieldKind::Color4: return 4;
    default:                                        return 1;
    }
}
inline uint32_t MatFieldBytes(MatFieldKind k) { return 4u * MatFieldLanes(k); }
inline bool     MatFieldIsFloat(MatFieldKind k) { return k != MatFieldKind::U32 && k != MatFieldKind::Bool; }

struct MatFieldSpec {
    const char*  key    = nullptr;   // json-ключ поля в materials.json И (при пустом label) подпись в UI
    const char*  label  = nullptr;   // подпись в инспекторе; nullptr → key
    MatFieldKind kind   = MatFieldKind::F32;
    uint32_t     offset = 0;         // байтовое смещение в блобе (offsetof — см. макрос MAT_FIELD)

    // Диапазон: драг/слайдер в UI и, при clamp_on_load, жёсткий кламп на загрузке. lo==hi → не задан.
    float lo = 0, hi = 0;
    float speed = 0.01f;             // шаг драга в UI
    bool  clamp_on_load = false;
    bool  ui_readonly   = false;     // видно, но не редактируется (производное/служебное поле)

    // Единственная фабрика (как FieldSpec::Num у компонентов): ключ+смещение обычно дают макросом
    //   MatFieldSpec::Num(MAT_FIELD(MyParams, metallic), MatFieldKind::F32, 0, 1)
    static MatFieldSpec Num(const char* key, uint32_t offset, MatFieldKind kind,
                            float lo = 0, float hi = 0, float speed = 0.01f)
    {
        MatFieldSpec f;
        f.key = key; f.offset = offset; f.kind = kind;
        f.lo = lo; f.hi = hi; f.speed = speed;
        return f;
    }

    // Модификаторы-цепочки (как .Clamp()/.ReadOnly() у FieldSpec)
    MatFieldSpec&& Label(const char* l) && { label = l;            return std::move(*this); }
    MatFieldSpec&& Clamp()              && { clamp_on_load = true; return std::move(*this); }
    MatFieldSpec&& ReadOnly()           && { ui_readonly   = true; return std::move(*this); }

    const char* UiLabel() const { return label ? label : key; }
    uint32_t    Bytes()   const { return MatFieldBytes(kind); }
};

// Ключ + смещение поля одной записью: MAT_FIELD(OpaqueMaterialParams, metallic) → "metallic", 32
#define MAT_FIELD(T, member) #member, (uint32_t)offsetof(T, member)

struct MaterialParamsSpec {
    std::string     name;                                  // "Opaque" — ключ в materials.json И подпись в дропдауне
    std::type_index type = std::type_index(typeid(void));  // typeid(T) — типизированный доступ (MaterialParamsAs<T>)
    size_t          size = 0;                              // sizeof(T) — размер блоба
    // Байты T{}: истина о дефолтах — member-инициализаторы самой структуры, а не дубль в схеме.
    // С них стартует загрузка (недостающий в файле ключ просто остаётся дефолтным).
    std::vector<uint8_t>      defaults;
    std::vector<MatFieldSpec> fields;
    // Escape hatch для невыразимого схемой (аналог custom_save у компонентов): если задан —
    // инспектор зовёт его вместо generic-рендерера. Блоб тип-стёрт до void* (ImGui сюда не течёт).
    std::function<void(void* blob)> custom_edit;

    const MatFieldSpec* Field(const char* key) const {
        for (const MatFieldSpec& f : fields) if (f.key && key && std::strcmp(f.key, key) == 0) return &f;
        return nullptr;
    }
};

// Реестр типов params. Ключуется ИМЕНЕМ (оно же идёт в файл) + typeid — ровно как ComponentSpecRegistry.
class MaterialParamsSpecRegistry {
public:
    static MaterialParamsSpecRegistry& Get();

    // Идемпотентно по имени (повторная регистрация игнорируется — как у компонентов).
    // Поля, вылезающие за sizeof(T), отбрасываются с ошибкой в лог: схема врёт про раскладку.
    void Register(MaterialParamsSpec s);

    const MaterialParamsSpec* ByName(const std::string& name) const;   // загрузка/сохранение/UI
    const MaterialParamsSpec* ByType(std::type_index t) const;         // типизированный доступ
    // Все типы в порядке регистрации — дропдаун «Type» в инспекторе материала.
    const std::vector<MaterialParamsSpec>& All() const { return specs_; }

private:
    std::vector<MaterialParamsSpec>             specs_;
    std::unordered_map<std::string, size_t>     by_name_;
    std::unordered_map<std::type_index, size_t> by_type_;
};

// Спека из типа: kind/size/defaults выводятся из T, руками пишется только имя и схема полей.
template<class T>
MaterialParamsSpec MakeMaterialParamsSpec(std::string name, std::vector<MatFieldSpec> fields)
{
    static_assert(std::is_trivially_copyable_v<T>, "params-блоб копируется байтами в cbuffer");
    static_assert(sizeof(T) % 16 == 0, "раскладка cbuffer обязана быть кратна 16 байтам");
    MaterialParamsSpec s;
    s.name = std::move(name);
    s.type = std::type_index(typeid(T));
    s.size = sizeof(T);
    const T d{};
    s.defaults.resize(sizeof(T));
    std::memcpy(s.defaults.data(), &d, sizeof(T));
    s.fields = std::move(fields);
    return s;
}

// Имя зарегистрированного типа по typeid; "" + ошибка в лог, если тип не регистрировали
// (тогда блоб уедет в рендер, но UI не сможет его разобрать, а SaveScene — сохранить).
const std::string& MaterialParamsTypeName(std::type_index t);

// Указатель на поле в блобе; nullptr, если поле не влезает (рассинхрон схемы и блоба).
inline void* MatFieldPtr(std::vector<uint8_t>& blob, const MatFieldSpec& f)
{
    return (f.offset + f.Bytes() <= blob.size()) ? static_cast<void*>(blob.data() + f.offset) : nullptr;
}
inline const void* MatFieldPtr(const std::vector<uint8_t>& blob, const MatFieldSpec& f)
{
    return (f.offset + f.Bytes() <= blob.size()) ? static_cast<const void*>(blob.data() + f.offset) : nullptr;
}

// Тип-безопасная упаковка per-material факторов (T = раскладка cbuffer MaterialBlock) в
// Material::params. Имя типа берётся из реестра — тег и блоб не могут разойтись.
// Мутация байт на месте НЕ трогает адрес вектора → ключ texture-батча цел (правка без ребилда).
template<class T>
void SetMaterialParams(Material* m, const T& p)
{
    if (!m) return;
    m->params.resize(sizeof(T));
    std::memcpy(m->params.data(), &p, sizeof(T));
    m->params_type = MaterialParamsTypeName(std::type_index(typeid(T)));
}

// Типизированное чтение блоба: nullptr, если тип материала другой/не зарегистрирован/блоб короче.
template<class T>
const T* MaterialParamsAs(const Material& m)
{
    const MaterialParamsSpec* s = MaterialParamsSpecRegistry::Get().ByType(std::type_index(typeid(T)));
    if (!s || s->name != m.params_type || m.params.size() < sizeof(T)) return nullptr;
    return reinterpret_cast<const T*>(m.params.data());
}
template<class T>
T* MaterialParamsAs(Material& m)
{
    return const_cast<T*>(MaterialParamsAs<T>(static_cast<const Material&>(m)));
}

// Поставить материалу дефолтный блоб типа (смена типа в дропдауне инспектора).
void ApplyMaterialParamsSpec(Material* m, const MaterialParamsSpec& s);
// Снять params вовсе (пункт «(none)» — материалу без MaterialBlock в шейдере).
void ClearMaterialParams(Material* m);

// Регистрирует встроенные (движковые) типы params: Opaque, Transparent. Идемпотентна —
// повторный вызов ничего не дублирует. Звать один раз на старте движка (Engine::Init),
// рядом с RegisterBuiltinComponentSpecs().
void RegisterBuiltinMaterialParamsSpecs();
