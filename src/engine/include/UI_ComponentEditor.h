#pragma once
// Generic-редактор компонента по его схеме (ComponentSpec::fields): ОДИН рендерер на все
// зарегистрированные компоненты — поле объявлено в реестре один раз, виджет выводится по kind
// (F32→Drag, U32→DragInt, Bool→Checkbox, Angle→SliderAngle в градусах, Str→Input/Label).
// Конвенции группировки соседних F32-полей: r,g,b → ColorEdit3; <p>_x,_y,_z и x,y,z → DragFloat3.
// Правка пишет напрямую в колонки архетипа (та же модель, что у прежних рукописных
// свет-редакторов) и дёргает spec.after_edit. ui_readonly-поля показываются задизейбленными.
// Потребители: инспектор сущности (цикл по компонентам архетипа) и форма создания (фаза 3).
#include "ComponentSerializer.h"   // ComponentSpec/FieldSpec (EngineEcs; ImGui сюда не протекает)

namespace ui {
    // true — хотя бы одно поле изменено в этом кадре (after_edit уже вызван).
    bool DrawComponentFields(const ComponentSpec& spec, Archetype& arch, size_t row);
}
