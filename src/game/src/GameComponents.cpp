#include "PCH.h"
#include "GameComponents.h"
#include <cfloat>

void RegisterGameComponents()
{
    using enum FieldKind;
    auto& reg = ComponentSpecRegistry::Get();

    reg.Register({ .name = "Mass", .sig_type = typeid(MassComponent),
        .add_default = AddDefaultAoS<MassComponent>,
        .fields = { FieldSpec::Num("mass", F32, AOS_NUM(MassComponent, mass), 0, FLT_MAX) } });

    // Gravity: центр притяжения; ЕГО позиция (Transform той же сущности) и есть центр.
    reg.Register({ .name = "Gravity", .sig_type = typeid(GravityComponent),
        .add_default = AddDefaultAoS<GravityComponent>,
        .fields = { FieldSpec::Num("gm", F32, AOS_NUM(GravityComponent, gm), 0, FLT_MAX, 1.0f) } });
}
