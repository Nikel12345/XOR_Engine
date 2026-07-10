#pragma once
#include <string>
#include "imgui.h"   // ImVec2 (дефолтный аргумент DangerButton)

// Мелкие переиспользуемые визуальные элементы редактора: делятся браузером ассетов
// и инспектором. Логики состояния тут нет — только рисование + возврат клика.
namespace ui {

// Служебный ассет = имя на "_" (напр. "_NoTextureDummy"). Общий фильтр браузера и
// дропдаунов текстур в инспекторе — одна галочка (g_show_internal) на всё.
inline bool IsInternalName(const std::string& n) { return !n.empty() && n[0] == '_'; }

// Тип ассета для плитки-превью — задаёт цвет + рисунок-затычку (реального превью пока нет:
// атласы движка — 2D_ARRAY, ImGui-конвейер их не сэмплит; см. DrawAssetIcon).
enum class AssetIcon { Texture, Model, Material, Shader, Compute, Vsd, Fsd, Csd, Generic };

// Плитка браузера ассетов: квадрат-превью (иконка-затычка по типу) + имя под ним, всё в группе
// (чтобы SameLine переносил их как единое целое). true при клике.
bool AssetTile(const char* name, bool selected, float size, AssetIcon icon = AssetIcon::Generic);

// Плитка-«плюс» того же размера, что превью: «+» рисуем сами почти во весь квадрат
// (кнопочный текст "+" был бы крохотным). true при клике; что именно создаём — за вызывающим.
bool PlusTile(float size);

// Красная кнопка деструктивного действия (Delete и т.п.).
bool DangerButton(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f));

} // namespace ui
