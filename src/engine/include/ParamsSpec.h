#pragma once
// Реестр ТИПОВ параметров-блоба cbuffer'а — прямое зеркало ComponentSerializer.h, но для сырых
// байт. Тип ДЕКЛАРИРУЕТ схему полей (fields), а редактор в инспекторе и запись/чтение манифеста —
// генерируемые интерпретаторы этой схемы: поле объявляется ОДИН РАЗ, диапазоны и дефолты не
// расходятся между файлом и UI.
//
// Доменов у блоба два, и у каждого свой экземпляр реестра (см. Materials()/Passes()):
//   Material::params          — фактор материала; тип ВЫБИРАЕТ пользователь в дропдауне;
//   (Render|Compute)PassStep::state — состояние прохода (порог/сила bloom, номер камеры);
//                               тип задан самим проходом.
// Механика у них общая целиком, отличается только владелец блоба и то, кто назначает тип.
//
// Регистрация ОТКРЫТАЯ и полностью симметрична компонентам: движок регистрирует свои типы в
// RegisterBuiltinMaterialParamsSpecs(), верхние слои (игра) — из своего кода одной записью,
// НЕ ПРАВЯ НИ ОДНОГО ФАЙЛА ДВИЖКА (раньше это было невозможно: тип-тег жил закрытым enum'ом
// MaterialParamsKind внутри движка, и его строковый round-trip — рукописным switch в Engine_Scene):
//
//   struct alignas(16) WaterParams { float tint[4]{0.2f,0.5f,0.9f,1}; float waveAmp = 0.1f, waveSpeed = 1, foam = 0, _pad = 0; };
//   ParamsSpecRegistry::Materials().Register(MakeParamsSpec<WaterParams>("Water", {
//       ParamsFieldSpec::Num(PARAMS_FIELD(WaterParams, tint),      ParamsFieldKind::Color4).Label("Tint"),
//       ParamsFieldSpec::Num(PARAMS_FIELD(WaterParams, waveAmp),   ParamsFieldKind::F32, 0, 1),
//       ParamsFieldSpec::Num(PARAMS_FIELD(WaterParams, waveSpeed), ParamsFieldKind::F32, 0, 4),
//   }));
//
// После этого тип сам появляется в дропдауне «Type» инспектора, его поля рисует generic-рендерер
// (ui::DrawParamsFields), а материалы с ним сохраняются/грузятся по ИМЕНАМ полей.
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
#include <SDL3/SDL_log.h>
#include "MaterialData.h"

// Вид поля: сколько 4-байтовых лейнов занимает в блобе и каким виджетом рисуется.
// Все лейны по 4 байта — как в cbuffer (bool в HLSL тоже 4 байта, на CPU держим uint32_t).
// Angle — радианы в блобе и в файле (как F32), слайдер UI в градусах; lo/hi у него — ГРАДУСЫ.
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
inline bool     ParamsFieldIsFloat(ParamsFieldKind k) { return k != ParamsFieldKind::U32 && k != ParamsFieldKind::Bool; }

struct ParamsFieldSpec {
    const char*  key    = nullptr;   // json-ключ поля в materials.json И (при пустом label) подпись в UI
    const char*  label  = nullptr;   // подпись в инспекторе; nullptr → key
    ParamsFieldKind kind   = ParamsFieldKind::F32;
    uint32_t     offset = 0;         // байтовое смещение в блобе (offsetof — см. макрос PARAMS_FIELD)

    // Диапазон: драг/слайдер в UI и, при clamp_on_load, жёсткий кламп на загрузке. lo==hi → не задан.
    float lo = 0, hi = 0;
    float speed = 0.01f;             // шаг драга в UI
    bool  clamp_on_load = false;
    bool  ui_readonly   = false;     // видно, но не редактируется (производное/служебное поле)

    // Единственная фабрика (как FieldSpec::Num у компонентов): ключ+смещение обычно дают макросом
    //   ParamsFieldSpec::Num(PARAMS_FIELD(MyParams, metallic), ParamsFieldKind::F32, 0, 1)
    static ParamsFieldSpec Num(const char* key, uint32_t offset, ParamsFieldKind kind,
                            float lo = 0, float hi = 0, float speed = 0.01f)
    {
        ParamsFieldSpec f;
        f.key = key; f.offset = offset; f.kind = kind;
        f.lo = lo; f.hi = hi; f.speed = speed;
        return f;
    }

    // Модификаторы-цепочки (как .Clamp()/.ReadOnly() у FieldSpec)
    ParamsFieldSpec&& Label(const char* l) && { label = l;            return std::move(*this); }
    ParamsFieldSpec&& Clamp()              && { clamp_on_load = true; return std::move(*this); }
    ParamsFieldSpec&& ReadOnly()           && { ui_readonly   = true; return std::move(*this); }

    const char* UiLabel() const { return label ? label : key; }
    uint32_t    Bytes()   const { return ParamsFieldBytes(kind); }
};

// Ключ + смещение поля одной записью: PARAMS_FIELD(OpaqueMaterialParams, metallic) → "metallic", 32
#define PARAMS_FIELD(T, member) #member, (uint32_t)offsetof(T, member)

struct ParamsSpec {
    std::string     name;                                  // "Opaque" — ключ в materials.json И подпись в дропдауне
    std::type_index type = std::type_index(typeid(void));  // typeid(T) — типизированный доступ (MaterialParamsAs<T>)
    size_t          size = 0;                              // sizeof(T) — размер блоба
    // Байты T{}: истина о дефолтах — member-инициализаторы самой структуры, а не дубль в схеме.
    // С них стартует загрузка (недостающий в файле ключ просто остаётся дефолтным).
    std::vector<uint8_t>      defaults;
    std::vector<ParamsFieldSpec> fields;
    // Escape hatch для невыразимого схемой (аналог custom_save у компонентов): если задан —
    // инспектор зовёт его вместо generic-рендерера. Блоб тип-стёрт до void* (ImGui сюда не течёт).
    std::function<void(void* blob)> custom_edit;

    const ParamsFieldSpec* Field(const char* key) const {
        for (const ParamsFieldSpec& f : fields) if (f.key && key && std::strcmp(f.key, key) == 0) return &f;
        return nullptr;
    }
};

// Реестр типов params. Ключуется ИМЕНЕМ (оно же идёт в файл) + typeid — ровно как ComponentSpecRegistry.
class ParamsSpecRegistry {
public:
    // ДВА независимых экземпляра одного реестра — по домену, где блоб живёт. Общий был бы ложью
    // о выборе: дропдаун «Type» инспектора материала перечисляет ВЕСЬ свой реестр, и тип
    // bloom-параметров оказался бы там предложением сделать материал блумом.
    static ParamsSpecRegistry& Materials();   // Material::params — тип выбирает пользователь
    static ParamsSpecRegistry& Passes();      // (Render|Compute)PassStep::state — тип задан проходом

    // Идемпотентно по имени (повторная регистрация игнорируется — как у компонентов).
    // Поля, вылезающие за sizeof(T), отбрасываются с ошибкой в лог: схема врёт про раскладку.
    void Register(ParamsSpec s);

    const ParamsSpec* ByName(const std::string& name) const;   // загрузка/сохранение/UI
    const ParamsSpec* ByType(std::type_index t) const;         // типизированный доступ
    // Все типы в порядке регистрации — дропдаун «Type» в инспекторе материала.
    // Реестру проходов не нужен: тип состояния задан самим проходом и не выбирается.
    const std::vector<ParamsSpec>& All() const { return specs_; }

private:
    std::vector<ParamsSpec>             specs_;
    std::unordered_map<std::string, size_t>     by_name_;
    std::unordered_map<std::type_index, size_t> by_type_;
};

// Спека из типа: kind/size/defaults выводятся из T, руками пишется только имя и схема полей.
template<class T>
ParamsSpec MakeParamsSpec(std::string name, std::vector<ParamsFieldSpec> fields)
{
    static_assert(std::is_trivially_copyable_v<T>, "params-блоб копируется байтами в cbuffer");
    static_assert(sizeof(T) % 16 == 0, "раскладка cbuffer обязана быть кратна 16 байтам");
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

// Имя зарегистрированного типа по typeid; "" + ошибка в лог, если тип не регистрировали
// (тогда блоб уедет в рендер, но UI не сможет его разобрать, а SaveScene — сохранить).
const std::string& MaterialParamsTypeName(std::type_index t);

// Указатель на поле в блобе; nullptr, если поле не влезает (рассинхрон схемы и блоба).
inline void* ParamsFieldPtr(std::vector<uint8_t>& blob, const ParamsFieldSpec& f)
{
    return (f.offset + f.Bytes() <= blob.size()) ? static_cast<void*>(blob.data() + f.offset) : nullptr;
}
inline const void* ParamsFieldPtr(const std::vector<uint8_t>& blob, const ParamsFieldSpec& f)
{
    return (f.offset + f.Bytes() <= blob.size()) ? static_cast<const void*>(blob.data() + f.offset) : nullptr;
}

// Запись блоба в ячейку sp: адресат — ИМЕННО эта sp материала, а не «материал вообще».
// Нет такой sp у материала → блобу некому ехать, поэтому это ошибка, а не тихий no-op.
// Не шаблон, чтобы заголовок не тянул SDL ради одного лога.
void SetMaterialParamsBlob(Material* m, const ShaderName& sp_name,
                           const void* data, size_t size, const std::string& type_name);

// Тип-безопасная упаковка per-sp факторов (T = раскладка cbuffer MaterialBlock этой sp).
// Имя типа берётся из реестра — тег и блоб не могут разойтись.
// Мутация байт на месте НЕ трогает адрес блоба → ключ texture-батча цел (правка без ребилда).
template<class T>
void SetMaterialParams(Material* m, const ShaderName& sp_name, const T& p)
{
    SetMaterialParamsBlob(m, sp_name, &p, sizeof(T), MaterialParamsTypeName(std::type_index(typeid(T))));
}

// Типизированное чтение блоба sp: nullptr, если sp нет у материала, тип другой/не зарегистрирован
// или блоб короче.
template<class T>
const T* MaterialParamsAs(const Material& m, const ShaderName& sp_name)
{
    const SpBinding* b = m.FindBinding(sp_name);
    if (!b || !b->params) return nullptr;
    const ParamsSpec* s = ParamsSpecRegistry::Materials().ByType(std::type_index(typeid(T)));
    if (!s || s->name != b->params_type || b->params->size() < sizeof(T)) return nullptr;
    return reinterpret_cast<const T*>(b->params->data());
}
template<class T>
T* MaterialParamsAs(Material& m, const ShaderName& sp_name)
{
    return const_cast<T*>(MaterialParamsAs<T>(static_cast<const Material&>(m), sp_name));
}

// Поставить ячейке дефолтный блоб типа (смена типа в дропдауне инспектора).
void ApplyMaterialParamsSpec(SpBinding* b, const ParamsSpec& s);
// Снять params вовсе (пункт «(none)» — sp без MaterialBlock в шейдере).
void ClearMaterialParams(SpBinding* b);

// ── Состояние прохода ((Render|Compute)PassStep::state) ──
// Второй домен блоба. То, что раньше было ЛОКАЛЬНОЙ структурой в теле прохода: часть полей
// тело переписывает каждый кадр (номер камеры, размеры раскладки), часть правит редактор и они
// просто лежат. В СХЕМУ идут только вторые — объявленное поле тем самым и редактируется, и (когда
// появится персистентность) сохраняется, а покадровому мусору не место ни там, ни там. Поэтому
// «скрытых» полей у схемы нет: не объявил — значит это не настройка.
// P шаблонный намеренно: заголовок не тянет RenderCommandData.h (и ничего движкового), а от шага
// нужна ровно пара полей state/state_type — та же по форме, что params/params_type у Material.
// Без имени схемы — состояние есть, но редактировать в нём НЕЧЕГО: все поля тело переписывает
// каждый кадр (номер камеры, размеры раскладки). Блоб тогда чистое хранилище, инспектор честно
// говорит, что крутить нечего. Схему заводят ровно тогда, когда в блобе появилось хоть одно поле,
// которое тело НЕ переписывает.
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

// Регистрирует встроенные (движковые) типы params: Opaque, Transparent. Идемпотентна —
// повторный вызов ничего не дублирует. Звать один раз на старте движка (Engine::Init),
// рядом с RegisterBuiltinComponentSpecs().
void RegisterBuiltinMaterialParamsSpecs();
