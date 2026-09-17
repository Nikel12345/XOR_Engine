#include "PCH.h"
#include "UI_ImGui.h"
#include "UI_Internal.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"
#include "ImGuizmo.h"
#include <filesystem>

namespace ui {
    Selection g_sel;
    bool      g_show_internal = false;
}

void UI_ImGui::Init(SDL_Window* win, SDL_GPUDevice* dev)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui_ImplSDL3_InitForSDLGPU(win);

    ImGui_ImplSDLGPU3_InitInfo init_info = {};
    init_info.Device = dev;
    init_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(dev, win);
    init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    ImGui_ImplSDLGPU3_Init(&init_info);

    io.Fonts->AddFontDefault();
#ifdef _WIN32
    {
        const char* sys_font = "C:/Windows/Fonts/segoeui.ttf";
        if (std::filesystem::exists(sys_font)) {
            ImFontConfig cfg;
            cfg.MergeMode = true;
            io.Fonts->AddFontFromFileTTF(sys_font, 0.0f, &cfg, io.Fonts->GetGlyphRangesCyrillic());
        } else {
            SDL_Log("ImGui font: '%s' not found - Cyrillic editor labels will render as '?'", sys_font);
        }
    }
#endif
}

void UI_ImGui::Shutdown()
{
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

std::vector<uint32_t> UI_ImGui::GetSelectedEntities()
{
    using namespace ui;
    std::vector<uint32_t> out;
    if (g_sel.kind == SelKind::Entity) {
        out.push_back(g_sel.entity);
        g_sel.kind = SelKind::None;
    }
    return out;
}

void UI_ImGui::SetSelectedEntities(const std::vector<uint32_t>& entities)
{
    using namespace ui;
    if (entities.empty()) return;
    g_sel.entity = entities.front();
    g_sel.index  = -1;
    g_sel.kind   = SelKind::Entity;
}

bool UI_ImGui::WantCaptureMouse()    { return ImGui::GetIO().WantCaptureMouse; }
bool UI_ImGui::WantCaptureKeyboard() { return ImGui::GetIO().WantCaptureKeyboard; }

void UI_ImGui::ProcessEvent(const SDL_Event& event)
{
    ImGui_ImplSDL3_ProcessEvent(&event);
}

void UI_ImGui::Iterate(EngineContext* ctx)
{
    ImGuizmo::BeginFrame();

    SetupDockspace();
    DrawHierarchy(ctx);
    DrawInspector(ctx);
    DrawAssetBrowser(ctx);

    DrawGizmo(ctx);
}

void UI_ImGui::SetupDockspace()
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGuiID dockspace_id = ImGui::GetID("EditorDockSpaceV2");

    static bool checked = false;
    if (!checked) {
        checked = true;
        if (ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
            ImGui::DockBuilderRemoveNode(dockspace_id);
            ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspace_id, vp->Size);

            ImGuiID center = dockspace_id;
            ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down,  0.30f, nullptr, &center);
            ImGuiID left   = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left,  0.20f, nullptr, &center);
            ImGuiID right  = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.30f, nullptr, &center);

            ImGui::DockBuilderDockWindow("Hierarchy", left);
            ImGui::DockBuilderDockWindow("Inspector", right);
            ImGui::DockBuilderDockWindow("Assets",    bottom);
            ImGui::DockBuilderFinish(dockspace_id);
        }
    }

    ImGui::DockSpaceOverViewport(dockspace_id, vp, ImGuiDockNodeFlags_PassthruCentralNode);
}
