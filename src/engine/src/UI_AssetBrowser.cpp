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
#include "InputManager.h"
#include "InputCommands.h"
#include "MaterialParamsSpec.h"
#include <functional>

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
        // SelKind → иконка-затычка плитки (цвет + рисунок по типу ассета).
        auto icon_of = [](SelKind k) {
            switch (k) {
            case SelKind::Texture:  return AssetIcon::Texture;
            case SelKind::Model:    return AssetIcon::Model;
            case SelKind::Material: return AssetIcon::Material;
            case SelKind::Shader:   return AssetIcon::Shader;
            case SelKind::Compute:  return AssetIcon::Compute;
            case SelKind::Vsd:      return AssetIcon::Vsd;
            case SelKind::Fsd:      return AssetIcon::Fsd;
            case SelKind::Csd:      return AssetIcon::Csd;
            default:                return AssetIcon::Generic;
            }
        };

        // Превью текстуры ПО ИМЕНИ: ячейка превью-атласа подсистемы PreviewPacker. Хэндл не нужен —
        // это и снимает моргание при LoadScene: пока sim декодит файл (хэндла ещё нет), слот превью
        // по имени жив, и плитка показывает прежнюю картинку. Невалидный UV → плитка нарисует затычку.
        TextureManager* tm = ctx->GetTextureManager();
        auto texture_preview = [&](const std::string& texName) -> TilePreview
        {
            TilePreview pv{};
            PreviewPacker::UV uv = tm->GetPreviewUV(texName);
            if (!uv.valid) return pv;
            pv.tex = (ImTextureID)(intptr_t)tm->GetPreviewAtlasTexture();
            pv.uv0 = ImVec2(uv.u0, uv.v0);
            pv.uv1 = ImVec2(uv.u1, uv.v1);
            return pv;
        };
        // Превью материала — три исхода, различимых с одного взгляда на список:
        //   albedo есть и резолвится → его превью с тинтом baseColor (Opaque);
        //   albedo НАЗНАЧЕН, но битый (удалён/переименован) → превью _NoTextureDummy БЕЗ тинта —
        //     маркер «тут дырка», как и в самом рендере;
        //   albedo-слота нет вообще (нетекстурный материал) → обычная затычка-сфера.
        auto material_preview = [&](const std::string& matName) -> TilePreview
        {
            TilePreview pv{};
            auto mit = ctx->GetMaterialManager()->GetMaterials().find(matName);
            if (mit == ctx->GetMaterialManager()->GetMaterials().end() || !mit->second) return pv;
            const Material* m = mit->second.get();
            auto tit = m->textures.find(TextureSlotRole::Albedo);
            if (tit == m->textures.end()) return pv;                // безальбедный → затычка
            pv = texture_preview(tit->second);
            if (pv.tex) {                                           // настоящий albedo → тинт цветом материала
                // Тинт берём из ПЕРВОГО цветового поля схемы типа params (у Opaque это baseColor,
                // у типа из кода игры — его собственный цвет). Нет цветовых полей / тип не
                // зарегистрирован → превью без тинта. Раскладку тут не знаем и знать не должны.
                if (const MaterialParamsSpec* s = MaterialParamsSpecRegistry::Get().ByName(m->params_type))
                    for (const MatFieldSpec& f : s->fields) {
                        if (f.kind != MatFieldKind::Color3 && f.kind != MatFieldKind::Color4) continue;
                        if (const void* fp = MatFieldPtr(m->params, f)) {
                            const float* c = static_cast<const float*>(fp);
                            pv.tint = ImVec4(c[0], c[1], c[2], 1.0f);
                        }
                        break;
                    }
            }
            else pv = texture_preview("_NoTextureDummy");           // битая ссылка → dummy, БЕЗ тинта
            return pv;
        };

        // Общая раскладка плиток: переносим ряд, когда следующая не влезает по ширине.
        // preview_of (опционально) — резолвер картинки-превью по имени; пустой tex → затычка.
        auto tiles = [&](SelKind kind, bool withNew, auto&& onNew, auto&& for_each_name,
                         std::function<TilePreview(const std::string&)> preview_of = {})
        {
            const AssetIcon icon = icon_of(kind);
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
                TilePreview pv{};
                if (preview_of) pv = preview_of(name);
                if (AssetTile(name.c_str(), selected, tile, icon, pv.tex ? &pv : nullptr)) {
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
                [&](auto&& emit) { for (auto& [name, mat] : ctx->GetMaterialManager()->GetMaterials()) emit(name); },
                material_preview);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Textures")) {
            tiles(SelKind::Texture, true,
                [&]{ g_sel = Selection{}; g_sel.kind = SelKind::Texture; g_sel.name = ""; },   // + = форма новой текстуры
                [&](auto&& emit) { for (auto& [name, h] : ctx->GetTextureManager()->GetTextureHandles()) emit(name); },
                texture_preview);
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
