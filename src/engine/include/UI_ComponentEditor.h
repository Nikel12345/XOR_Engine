#pragma once
#include "ComponentSerializer.h"
#include "ParamsSpec.h"

class EngineContext;

namespace ui {
    // Черновик (kNoEntity) UI-поток правит напрямую: он вне дерева батчей. У живой энтити
    // правки, меняющие состав батчей, обязаны идти командой в sim.
    struct EditTarget {
        static constexpr Entity kNoEntity = static_cast<Entity>(-1);

        EngineContext* ctx    = nullptr;
        Entity         entity = kNoEntity;

        bool live() const { return entity != kNoEntity; }
    };

    void DrawEntityComponents(const EditTarget& target, Archetype& arch, size_t row);

    // true — поле изменено в этом кадре. После прямой записи after_edit уже вызван; у полей,
    // ушедших командой, его вызовет хендлер.
    bool DrawComponentFields(const EditTarget& target, const ComponentSpec& spec,
                             Archetype& arch, size_t row);

    bool DrawParamsFields(const ParamsSpec& spec, std::vector<uint8_t>& blob);
}
