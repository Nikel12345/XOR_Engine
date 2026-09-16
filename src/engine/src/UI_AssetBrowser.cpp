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
#include "PassManager.h"
#include "InputCommands.h"
#include "ParamsSpec.h"
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
            case SelKind::Pass:     return AssetIcon::Compute;   // отдельной иконки у прохода нет
            default:                return AssetIcon::Generic;
            }
        };

        // Превью текстуры ПО ЯЧЕЙКЕ реестра: слот превью-атласа подсистемы PreviewPacker. Хэндл не
        // нужен — это и снимает моргание при LoadScene: пока sim декодит файл (хэндла ещё нет), слот
        // превью у ячейки жив, и плитка показывает прежнюю картинку. Невалидный UV → затычка.
        TextureManager* tm = ctx->GetTextureManager();
        auto texture_preview = [&](TextureId id) -> TilePreview
        {
            TilePreview pv{};
            PreviewPacker::UV uv = tm->GetPreviewUV(id);
            if (!uv.valid) return pv;
            pv.tex = (ImTextureID)(intptr_t)tm->GetPreviewAtlasTexture();
            pv.uv0 = ImVec2(uv.u0, uv.v0);
            pv.uv1 = ImVec2(uv.u1, uv.v1);
            return pv;
        };
        // Превью материала — три исхода, различимых с одного взгляда на список:
        //   albedo есть и резолвится → его превью с тинтом baseColor (Opaque);
        //   albedo НАЗНАЧЕН, но битый (удалён/переименован) → превью NoTextureDummy БЕЗ тинта —
        //     маркер «тут дырка», как и в самом рендере;
        //   albedo-слота нет вообще (нетекстурный материал) → обычная затычка-сфера.
        auto material_preview = [&](const std::string& matName) -> TilePreview
        {
            TilePreview pv{};
            const Material* m = ctx->GetMaterialManager()->GetMaterial(ctx->GetMaterialManager()->MaterialIdOf(matName));
            if (!m) return pv;
            auto tit = m->textures.find(TextureSlotRole::Albedo);
            if (tit == m->textures.end() || tit->second.empty()) return pv;   // безальбедный → затычка
            pv = texture_preview(tit->second[0]);                   // дефолт слота: варианты плитка не показывает
            if (pv.tex) {                                           // настоящий albedo → тинт цветом материала
                // Тинт берём из ПЕРВОГО цветового поля схемы типа params (у Opaque это baseColor,
                // у типа из кода игры — его собственный цвет). Нет цветовых полей / тип не
                // зарегистрирован → превью без тинта. Раскладку тут не знаем и знать не должны.
                // Params теперь у каждой sp свои — берём ПЕРВУЮ ячейку с цветовым полем
                // (обычно единственная «цветная» sp материала: Lit/LitColor).
                for (const SpBinding& b : m->shader_programs) {
                    if (!b.params || b.params->empty()) continue;
                    const ParamsSpec* s = ParamsSpecRegistry::Materials().ByName(b.params_type);
                    if (!s) continue;
                    bool tinted = false;
                    for (const ParamsFieldSpec& f : s->fields) {
                        if (f.kind != ParamsFieldKind::Color3 && f.kind != ParamsFieldKind::Color4) continue;
                        if (const void* fp = ParamsFieldPtr(*b.params, f)) {
                            const float* c = static_cast<const float*>(fp);
                            pv.tint = ImVec4(c[0], c[1], c[2], 1.0f);
                            tinted = true;
                        }
                        break;
                    }
                    if (tinted) break;
                }
            }
            else pv = texture_preview(tm->TextureIdOf("NoTextureDummy"));   // битая ссылка → dummy, БЕЗ тинта
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
                    MaterialManager* mm = ctx->GetMaterialManager();
                    std::string nm = "material";
                    for (int i = 1; mm->MaterialIdOf(nm); ++i) nm = "material_" + std::to_string(i);
                    g_sel = Selection{}; g_sel.kind = SelKind::Material; g_sel.name = nm;
                    ctx->GetInputManager()->PushCommand(CommandId::CreateMaterial, new CreateMaterialCmd{ nm });
                },
                [&](auto&& emit) { const MaterialRegistry& reg = ctx->GetMaterialManager()->Materials();
                                   for (int32_t i = 0; i < reg.Count(); ++i) {
                                       const MaterialCell& c = reg.At(i);
                                       if (c.object && (g_show_internal || !HasTag(c.object->tags, ResourceTag::System))) emit(c.name);
                                   } },
                material_preview);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Textures")) {
            tiles(SelKind::Texture, true,
                [&]{ g_sel = Selection{}; g_sel.kind = SelKind::Texture; g_sel.name = ""; },   // + = форма новой текстуры
                [&](auto&& emit) { const TextureRegistry& reg = ctx->GetTextureManager()->Textures();
                                   for (int32_t i = 0; i < reg.Count(); ++i) {
                                       const TextureCell& c = reg.At(i);
                                       if (c.object && (g_show_internal || !HasTag(c.object->tags, ResourceTag::System))) emit(c.name);
                                   } },
                [&](const std::string& n) { return texture_preview(tm->TextureIdOf(n)); });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Models")) {
            tiles(SelKind::Model, true,
                [&]{ g_sel = Selection{}; g_sel.kind = SelKind::Model; g_sel.name = ""; },   // + = форма новой модели
                [&](auto&& emit) { const ModelRegistry& reg = ctx->GetModelManager()->Models();
                                   for (int32_t i = 0; i < reg.Count(); ++i) { const ModelCell& c = reg.At(i); if (!c.object) continue; const std::string& name = c.name; const auto& m = c.object;
                                       if (g_show_internal || !HasTag(m->tags, ResourceTag::System)) emit(name); } });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Shaders")) {                                        // graphics sp (создание/правка в UI)
            tiles(SelKind::Shader, true,
                [&]{ g_sel = Selection{}; g_sel.kind = SelKind::Shader; g_sel.name = ""; },   // + = форма новой sp
                [&](auto&& emit) { for (auto& [name, sp] : ctx->GetShaderManager()->GetShaderPrograms())
                                       if (g_show_internal || !HasTag(sp->tags, ResourceTag::System)) emit(name); });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Compute")) {                                        // compute sp
            tiles(SelKind::Compute, false, []{},
                [&](auto&& emit) { for (auto& slot : ctx->GetShaderManager()->GetComputeShaderPrograms())
                                       if (slot.program && (g_show_internal || !HasTag(slot.program->tags, ResourceTag::System))) emit(slot.name); });
            ImGui::EndTabItem();
        }
        // Шаги кадра в порядке исполнения. Редактируется их state (см. ComputePassStep::state)
        // тем же generic-рендерером схемы, что и params материала. Блит-шаги не показываем: у них
        // нет ни шейдера, ни пуша — состоянию неоткуда взяться.
        // Имя — КЛЮЧ РЕЕСТРА (обход отдаёт пару), а не debug_name шага.
        if (ImGui::BeginTabItem("Passes")) {
            PassManager* pmgr = ctx->GetPassManager();
            std::vector<std::pair<int, const std::string*>> ordered;   // pass_index → имя
            for (const auto& [n, st] : pmgr->GetComputePrepasses()) ordered.push_back({ st->pass_index, &n });
            for (const auto& [n, st] : pmgr->GetRenderPasses())     ordered.push_back({ st->pass_index, &n });
            for (const auto& [n, st] : pmgr->GetComputePasses())    ordered.push_back({ st->pass_index, &n });
            std::sort(ordered.begin(), ordered.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
            tiles(SelKind::Pass, false, []{},
                [&](auto&& emit) { for (const auto& [idx, name] : ordered) emit(*name); },
                {});
            ImGui::EndTabItem();
        }
        // Именованные шейдер-данные (vs/fs/cs) — только список; редактирование (пути) появится позже.
        if (ImGui::BeginTabItem("VS")) {
            tiles(SelKind::Vsd, true,
                [&]{ g_sel = Selection{}; g_sel.kind = SelKind::Vsd; g_sel.name = ""; },   // + = форма нового vs
                [&](auto&& emit) { const VertexShaderRegistry& reg = ctx->GetShaderManager()->VertexShaders();
                                   for (int32_t i = 0; i < reg.Count(); ++i) {
                                       const VertexShaderCell& c = reg.At(i);
                                       if (c.object && (g_show_internal || !HasTag(c.object->tags, ResourceTag::System))) emit(c.name);
                                   } });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("FS")) {
            tiles(SelKind::Fsd, true,
                [&]{ g_sel = Selection{}; g_sel.kind = SelKind::Fsd; g_sel.name = ""; },
                [&](auto&& emit) { const FragmentShaderRegistry& reg = ctx->GetShaderManager()->FragmentShaders();
                                   for (int32_t i = 0; i < reg.Count(); ++i) {
                                       const FragmentShaderCell& c = reg.At(i);
                                       if (c.object && (g_show_internal || !HasTag(c.object->tags, ResourceTag::System))) emit(c.name);
                                   } });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("CS")) {
            tiles(SelKind::Csd, true,
                [&]{ g_sel = Selection{}; g_sel.kind = SelKind::Csd; g_sel.name = ""; },
                [&](auto&& emit) { const ComputeShaderRegistry& reg = ctx->GetShaderManager()->ComputeShaders();
                                   for (int32_t i = 0; i < reg.Count(); ++i) {
                                       const ComputeShaderCell& c = reg.At(i);
                                       if (c.object && (g_show_internal || !HasTag(c.object->tags, ResourceTag::System))) emit(c.name);
                                   } });
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}
