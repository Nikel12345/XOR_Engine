#pragma once
#include <unordered_map>
#include <vector>
#include <string>
#include <cstdint>
#include "ShaderTypes.h"
#include "Aliases.h"

struct Material {
    // Пользовательский ресурс ссылается на другой ТОЛЬКО по имени (правило редактора): не
    // указатель/weak_ptr, а имя, резолвится лениво в BatchBuilder на сборке батчей (TextureManager
    // по имени → TextureHandle). Промах (текстура удалена/переименована/ещё не создана) → nullptr →
    // dummy в батче. В отличие от weak_ptr, имя ПЕРЕЖИВАЕТ delete+recreate под тем же именем: ссылка
    // перепривязывается на новый хэндл при следующей пересборке, без ручной инвалидации.
    std::unordered_map<TextureSlotRole, TextureName> textures;
    std::vector<ShaderName> shader_programs;   // имена sp (материал может жить в нескольких пассах)

    // Непрозрачный блоб per-material факторов (alpha, baseColorFactor, metallic, ...). Layout
    // задаёт автор шейдера (его cbuffer); движок видит только байты и пушит их как есть.
    // Адрес &params стабилен (Material живёт под unique_ptr в MaterialManager) → им же
    // ключуется texture-батч: мутация байт на месте НЕ меняет ключ (твики в рантайме без
    // перестройки дерева), а разные материалы дают разные адреса (не схлопываются).
    std::vector<uint8_t> params;
    // ИМЯ типа params в реестре ParamsSpecRegistry (см. ParamsSpec.h) — тег ТОЛЬКО
    // для тулинга: по нему инспектор находит схему полей, а Save/LoadScene пишет и читает их
    // по-именно. Рендер тег не читает (params для него — непрозрачные байты). Пусто = у материала
    // нет params (шейдер без MaterialBlock). Строка, а не enum, именно ради открытой регистрации:
    // тип, объявленный в коде игры, называет себя сам — движок для этого не правится.
    // Выставляется автоматически из typeid(T) в SetMaterialParams.
    std::string          params_type;
    // Не писать в materials.json при SaveScene (кодовая инфраструктура — напр. debug_collider,
    // используемый только генератором debug-рамок). UI/пересоздание → false («тронул = сохраняемый»).
    bool dont_save = false;
};
