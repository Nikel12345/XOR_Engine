#include "PCH.h"
#include "UI_ImGui.h"
#include "UI_Internal.h"
#include "UI_Widgets.h"
#include "EngineContext.h"
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

void UI_ImGui::DrawAssetBrowser(EngineContext* ctx)
{
    ImGui::SetNextWindowBgAlpha(kPanelBgAlpha);
    ImGui::Begin("Assets");

    ImGui::Checkbox("Show internal (_)", &g_show_internal);
    ImGui::Separator();

    const float tile = 64.0f, pad = 8.0f;

    if (ImGui::BeginTabBar("AssetTabs"))
    {
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
            case SelKind::Pass:     return AssetIcon::Compute;
            default:                return AssetIcon::Generic;
            }
        };

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
        auto material_preview = [&](const std::string& matName) -> TilePreview
        {
            TilePreview pv{};
            const Material* m = ctx->GetMaterialManager()->GetMaterial(ctx->GetMaterialManager()->MaterialIdOf(matName));
            if (!m) return pv;
            auto tit = m->textures.find(TextureSlotRole::Albedo);
            if (tit == m->textures.end() || tit->second.empty()) return pv;
            pv = texture_preview(tit->second[0]);
            if (pv.tex) {
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
            else pv = texture_preview(tm->TextureIdOf("NoTextureDummy"));
            return pv;
        };

        auto tiles = [&](SelKind kind, bool withNew, auto&& onNew, auto&& for_each_name,
                         std::function<TilePreview(const std::string&)> preview_of = {})
        {
            const AssetIcon icon = icon_of(kind);
            float avail = ImGui::GetContentRegionAvail().x;
            int per_row = std::max(1, static_cast<int>(avail / (tile + pad)));
            int col = 0;
            auto step = [&]{ if (++col % per_row != 0) ImGui::SameLine(); };

            if (withNew) {
                if (PlusTile(tile)) onNew();
                step();
            }

            for_each_name([&](const std::string& name)
            {
                bool selected = (g_sel.kind == kind && g_sel.name == name);
                TilePreview pv{};
                if (preview_of) pv = preview_of(name);
                if (AssetTile(name.c_str(), selected, tile, icon, pv.tex ? &pv : nullptr)) {
                    if (selected) g_sel = Selection{};
                    else { g_sel = Selection{}; g_sel.kind = kind; g_sel.name = name; }
                }
                step();
            });
        };

        if (ImGui::BeginTabItem("Materials")) {
            tiles(SelKind::Material, true,
                [&]{
                    MaterialManager* mm = ctx->GetMaterialManager();
                    std::string nm = "material";
                    for (int i = 1; mm->MaterialIdOf(nm); ++i) nm = "material_" + std::to_string(i);
                    g_sel = Selection{}; g_sel.kind = SelKind::Material; g_sel.name = nm;
                    cmd::Push<CommandId::CreateMaterial>(ctx->GetInputManager(), nm);
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
                [&]{ g_sel = Selection{}; g_sel.kind = SelKind::Texture; g_sel.name = ""; },
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
                [&]{ g_sel = Selection{}; g_sel.kind = SelKind::Model; g_sel.name = ""; },
                [&](auto&& emit) { const ModelRegistry& reg = ctx->GetModelManager()->Models();
                                   for (int32_t i = 0; i < reg.Count(); ++i) { const ModelCell& c = reg.At(i); if (!c.object) continue; const std::string& name = c.name; const auto& m = c.object;
                                       if (g_show_internal || !HasTag(m->tags, ResourceTag::System)) emit(name); } });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Shaders")) {
            tiles(SelKind::Shader, true,
                [&]{ g_sel = Selection{}; g_sel.kind = SelKind::Shader; g_sel.name = ""; },
                [&](auto&& emit) { ShaderProgramRegistry& reg = ctx->GetShaderManager()->ShaderPrograms();
                                   for (int32_t i = 0; i < reg.Count(); ++i) {
                                       const ShaderProgramCell& c = reg.At(i);
                                       if (c.object && (g_show_internal || !HasTag(c.object->tags, ResourceTag::System))) emit(c.name);
                                   } });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Compute")) {
            tiles(SelKind::Compute, false, []{},
                [&](auto&& emit) { ComputeProgramRegistry& reg = ctx->GetShaderManager()->ComputePrograms();
                                   for (int32_t i = 0; i < reg.Count(); ++i) {
                                       const ComputeProgramCell& c = reg.At(i);
                                       if (c.object && (g_show_internal || !HasTag(c.object->tags, ResourceTag::System))) emit(c.name);
                                   } });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Passes")) {
            PassManager* pmgr = ctx->GetPassManager();
            std::vector<std::pair<int, const std::string*>> ordered;
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
        if (ImGui::BeginTabItem("VS")) {
            tiles(SelKind::Vsd, true,
                [&]{ g_sel = Selection{}; g_sel.kind = SelKind::Vsd; g_sel.name = ""; },
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
