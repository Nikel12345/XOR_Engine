#pragma once
#include <string>
#include "BaseComponents.h"

// Разделяемое между панелями редактора состояние. Панели живут в разных TU
// (UI_Hierarchy / UI_AssetBrowser / UI_Inspector), но общаются через один выбор
// и общие флаги — поэтому они здесь, а не в анонимном namespace одного .cpp.
namespace ui {

// ---- Выделение редактора: что сейчас показывает Inspector ----
// Клик в Hierarchy или по плитке ассета кладёт сюда «вид + идентификатор», а Inspector
// по нему решает, что рисовать. Свет НЕ отдельный вид: он такая же сущность (Entity).
// Виды — только «сущность vs ресурс». Shader = graphics sp, Compute = compute sp
// (2 типа, отдельные вкладки-фильтры).
enum class SelKind { None, Entity, Camera, Material, Texture, Model, Shader, Compute,
                     Vsd, Fsd, Csd,     // именованные шейдер-данные (список; своё окно редактирования — позже)
                     UINode };          // узел дерева UI_Yoga (UI-вкладка; двигается гизмо, см. UI_Yoga.h)

struct Selection {
    SelKind     kind    = SelKind::None;
    Entity      entity  = static_cast<Entity>(-1);  // для Entity (включая свет)
    int         index   = -1;                       // для Camera (порядковый)
    std::string name;                               // для Material / Texture / Model
    uint32_t    ui_node = 0xFFFFFFFFu;              // для UINode (handle = UI_Yoga::Node; kInvalid по умолчанию)
};

// Определения — в UI_ImGui.cpp (якорный TU панелей).
extern Selection g_sel;          // текущий выбор (кого показывает Inspector / держит гизмо)
extern bool      g_show_internal;// показывать служебные ассеты (имена на "_", напр. "_NoTextureDummy")

// Полупрозрачный фон панелей (0 = сквозь пустые места видно 3D-сцену).
constexpr float kPanelBgAlpha = 0.0f;

} // namespace ui
