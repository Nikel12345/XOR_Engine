#pragma once
// ЕДИНЫЙ вид энтити по компонентам — один и тот же и в инспекторе, и в форме создания.
// Раньше видов было два: рукописный в инспекторе и свой цикл в форме; теперь обе панели
// зовут DrawEntityComponents, а источник истины — схема компонента (ComponentSpec::fields):
// поле объявлено в реестре ОДИН раз.
//
// Виджет выводится по kind (F32→Drag, U32→DragInt, Bool→Checkbox, Angle→SliderAngle в
// градусах, Str→Input/Label). Конвенции группировки соседних F32-полей: r,g,b → ColorEdit3;
// <p>_x,_y,_z и x,y,z → DragFloat3. Правка пишет напрямую в колонки архетипа и дёргает
// spec.after_edit; ui_readonly-поля показываются задизейбленными.
#include "ComponentSerializer.h"
#include "ParamsSpec.h"

class EngineContext;

namespace ui {
    // Куда пишет редактор. entity == kNoEntity — staging-черновик формы создания: он не в
    // дереве батчей и не виден ни рендеру, ни дата-модулям, поэтому UI-поток правит его
    // монопольно и напрямую. Живая энтити (entity != kNoEntity) — те же поля, но правки,
    // меняющие СОСТАВ батчей (модель/материал/видимость), обязаны идти командой в sim.
    struct EditTarget {
        static constexpr Entity kNoEntity = static_cast<Entity>(-1);

        EngineContext* ctx    = nullptr;
        Entity         entity = kNoEntity;

        bool live() const { return entity != kNoEntity; }
    };

    // Секция на каждый компонент архетипа (порядок = порядок регистрации, детерминирован),
    // теги без данных — строкой внизу. Новый зарегистрированный компонент появляется в обеих
    // панелях сам, без правки UI.
    void DrawEntityComponents(const EditTarget& target, Archetype& arch, size_t row);

    // Поля одного компонента по его схеме. true — хотя бы одно поле изменено в этом кадре
    // (после прямой записи after_edit уже вызван; поля с FieldSpec::cmd у живой энтити уходят
    // командой в sim — там же и запись, и after_edit).
    bool DrawComponentFields(const EditTarget& target, const ComponentSpec& spec,
                             Archetype& arch, size_t row);

    // Тот же generic-редактор, но по схеме ТИПА params материала: поля адресуются смещением
    // в блобе (он плоский POD), правка идёт in-place — адрес вектора не меняется, значит ключ
    // texture-батча цел и пересборка дерева не нужна. true — что-то изменено в этом кадре.
    bool DrawParamsFields(const ParamsSpec& spec, std::vector<uint8_t>& blob);
}
