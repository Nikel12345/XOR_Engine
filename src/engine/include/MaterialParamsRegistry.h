#pragma once
#include <string>
#include <vector>
#include <functional>
#include "MaterialData.h"   // MaterialParamsKind

class EngineContext;
struct Material;

// Дескриптор ТИПА params материала для тулинга (UI-дропдаун "Kind"):
//   kind         — тег-идентификатор (движковый enum; см. MaterialData.h).
//   label        — подпись в дропдауне.
//   applyDefault — поставить дефолтный блоб типа на материал (обычно ctx->SetMaterialParams<T>(m, T{})).
//   edit         — НАРИСОВАТЬ поля этого типа в инспекторе (reinterpret блоба + ImGui-виджеты, in-place).
// Тело edit зовёт ImGui, но std::function стирает тип → сам заголовок остаётся ImGui-free; ImGui нужен
// лишь там, где дескриптор РЕГИСТРИРУЮТ (у нас — движок). Пустой edit допустим (напр. None) → инспектор
// откатится на сырые float.
struct MaterialParamsTypeDesc {
    MaterialParamsKind kind;
    std::string        label;
    std::function<void(EngineContext*, Material*)> applyDefault;
    std::function<void(Material*)>                 edit;
};

// Реестр типов params. Наполняется ОДИН раз на старте: билтины (RegisterBuiltinMaterialParamsTypes
// в Engine::Init) + пользовательские — из кода игры одной строкой, без правки движка:
//   MaterialParamsRegistry::Get().Register({ MyParams::kind, "MyKind",
//       [](EngineContext* ctx, Material* m){ ctx->SetMaterialParams(m, MyParams{}); } });
// «Полудинамика»: расширяемо в коде, но не рассчитано на переопределение в рантайме.
// Header-only inline-синглтон — не тянет отдельный .cpp / правку CMake. По тому же принципу,
// что ComponentSerializerRegistry, но с std::function-дескрипторами (меньше церемоний, чем наследование —
// а наследовать сам блоб params нельзя: это сырые байты для cbuffer, без vtable).
class MaterialParamsRegistry {
public:
    static MaterialParamsRegistry& Get() { static MaterialParamsRegistry inst; return inst; }

    void Register(MaterialParamsTypeDesc d) {
        for (auto& e : descs_) if (e.kind == d.kind) { e = std::move(d); return; }   // идемпотентно по kind
        descs_.push_back(std::move(d));
    }

    const std::vector<MaterialParamsTypeDesc>& All() const { return descs_; }

private:
    std::vector<MaterialParamsTypeDesc> descs_;
};
