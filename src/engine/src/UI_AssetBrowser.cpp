#include "PCH.h"
#include "UI_ImGui.h"
#include "UI_Internal.h"
#include "UI_Widgets.h"
#include "EngineContext.h"
// EngineContext.h держит менеджеры forward-декларациями — полные типы тянет этот TU.
#include "TextureManager.h"
#include "MaterialManager.h"
#include "ModelManager.h"
#include "ShaderManager.h"
#include "InputManager.h"   // PushCommand + CommandId
#include "InputCommands.h"  // CreateMaterialCmd и прочие payload-структуры

using namespace ui;

// Нижняя панель: браузер ассетов (вкладки Materials/Textures/Models/Shaders/Compute) — плитки
// с подписью-именем; клик → g_sel, правка уходит в Inspector.
void UI_ImGui::DrawAssetBrowser(EngineContext* ctx)
{
    ImGui::SetNextWindowBgAlpha(kPanelBgAlpha);
    ImGui::Begin("Assets");

    // Фильтр служебных ассетов: общий флаг (см. g_show_internal) — та же галочка действует и на
    // дропдауны текстур в инспекторе.
    ImGui::Checkbox("Show internal (_)", &g_show_internal);
    ImGui::Separator();

    const float tile = 64.0f, pad = 8.0f;

    if (ImGui::BeginTabBar("AssetTabs"))
    {
        // Общая раскладка плиток: переносим ряд, когда следующая не влезает по ширине.
        auto tiles = [&](SelKind kind, bool withNew, auto&& onNew, auto&& for_each_name)
        {
            float avail = ImGui::GetContentRegionAvail().x;
            int per_row = std::max(1, static_cast<int>(avail / (tile + pad)));
            int col = 0;
            auto step = [&]{ if (++col % per_row != 0) ImGui::SameLine(); };

            if (withNew) {
                // Плитка-«плюс». Действие — за onNew (у текстур форма, у материалов сразу команда создания).
                if (PlusTile(tile)) onNew();
                step();
            }

            for_each_name([&](const std::string& name)
            {
                if (!g_show_internal && IsInternalName(name)) return;   // фильтр служебных
                bool selected = (g_sel.kind == kind && g_sel.name == name);
                if (AssetTile(name.c_str(), selected, tile)) {
                    if (selected) g_sel = Selection{};                                 // повторный клик — снять
                    else { g_sel = Selection{}; g_sel.kind = kind; g_sel.name = name; }
                }
                step();
            });
        };

        if (ImGui::BeginTabItem("Materials")) {
            tiles(SelKind::Material, true,
                [&]{
                    // Свободное имя считаем в UI → сразу ставим выбор на создаваемый материал.
                    auto& mats = ctx->GetMaterialManager()->GetMaterials();
                    std::string nm = "material";
                    for (int i = 1; mats.count(nm); ++i) nm = "material_" + std::to_string(i);
                    g_sel = Selection{}; g_sel.kind = SelKind::Material; g_sel.name = nm;
                    ctx->GetInputManager()->PushCommand(CommandId::CreateMaterial, new CreateMaterialCmd{ nm });
                },
                [&](auto&& emit) { for (auto& [name, mat] : ctx->GetMaterialManager()->GetMaterials()) emit(name); });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Textures")) {
            tiles(SelKind::Texture, true,
                [&]{ g_sel = Selection{}; g_sel.kind = SelKind::Texture; g_sel.name = ""; },   // + = форма новой текстуры
                [&](auto&& emit) { for (auto& [name, h] : ctx->GetTextureManager()->GetTextureHandles()) emit(name); });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Models")) {
            tiles(SelKind::Model, true,
                [&]{ g_sel = Selection{}; g_sel.kind = SelKind::Model; g_sel.name = ""; },   // + = форма новой модели
                [&](auto&& emit) { for (auto& [name, m] : ctx->GetModelManager()->GetModels()) emit(name); });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Shaders")) {                                        // graphics sp (создание/правка в UI)
            tiles(SelKind::Shader, true,
                [&]{ g_sel = Selection{}; g_sel.kind = SelKind::Shader; g_sel.name = ""; },   // + = форма новой sp
                [&](auto&& emit) { for (auto& [name, sp] : ctx->GetShaderManager()->GetShaderPrograms()) emit(name); });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Compute")) {                                        // compute sp
            tiles(SelKind::Compute, false, []{},
                [&](auto&& emit) { for (auto& csp : ctx->GetShaderManager()->GetComputeShaderPrograms()) emit(csp->debug_name); });
            ImGui::EndTabItem();
        }
        // Именованные шейдер-данные (vs/fs/cs) — только список; редактирование (пути) появится позже.
        if (ImGui::BeginTabItem("VS")) {
            tiles(SelKind::Vsd, true,
                [&]{ g_sel = Selection{}; g_sel.kind = SelKind::Vsd; g_sel.name = ""; },   // + = форма нового vs
                [&](auto&& emit) { for (auto& [n, d] : ctx->GetShaderManager()->GetVertexShaders()) emit(n); });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("FS")) {
            tiles(SelKind::Fsd, true,
                [&]{ g_sel = Selection{}; g_sel.kind = SelKind::Fsd; g_sel.name = ""; },
                [&](auto&& emit) { for (auto& [n, d] : ctx->GetShaderManager()->GetFragmentShaders()) emit(n); });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("CS")) {
            tiles(SelKind::Csd, true,
                [&]{ g_sel = Selection{}; g_sel.kind = SelKind::Csd; g_sel.name = ""; },
                [&](auto&& emit) { for (auto& [n, d] : ctx->GetShaderManager()->GetComputeShaders()) emit(n); });
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}
