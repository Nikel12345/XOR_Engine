#include "PCH.h"
#include "UI_ImGui.h"
#include "UI_Internal.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"
#include "ImGuizmo.h"
#include <filesystem>

// Панели живут в отдельных TU (UI_Hierarchy / UI_AssetBrowser / UI_Inspector), но методы —
// все члены UI_ImGui, а их общее состояние объявлено в UI_Internal.h. Тут — «якорь»: определения
// этого состояния + кадровый оркестратор (Iterate) + первичная раскладка доков (SetupDockspace).

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
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;   // docking-ветка ImGui: окна можно стыковать (см. будущий DockSpace)

    ImGui_ImplSDL3_InitForSDLGPU(win);

    ImGui_ImplSDLGPU3_InitInfo init_info = {};
    init_info.Device = dev;
    init_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(dev, win);
    init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    ImGui_ImplSDLGPU3_Init(&init_info);

    // Шрифт ImGui: дефолтный ProggyClean покрывает только латиницу, поэтому кириллические подписи
    // редактора (инспектор/иерархия) без этого рисуются знаками «?». Догружаем кириллицу МЕРЖ-режимом
    // из системного шрифта: латиница остаётся дефолтной (при пересечении глифов побеждает первый
    // добавленный шрифт), из Segoe UI берутся только кириллические глифы. ImGui 1.92 растеризует по
    // требованию (backend выставляет RendererHasTextures) — ручная сборка атласа не нужна. Путь
    // системный → только Windows и только если файл реально есть (иначе просто оставляем дефолт).
    io.Fonts->AddFontDefault();
#ifdef _WIN32
    {
        const char* sys_font = "C:/Windows/Fonts/segoeui.ttf";
        if (std::filesystem::exists(sys_font)) {
            ImFontConfig cfg;
            cfg.MergeMode = true;   // доклеить в дефолтный шрифт, а не заменить его
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

// Обмен выбором с игрой (sim-поток) — контракт и модель потокобезопасности в заголовке.
std::vector<uint32_t> UI_ImGui::GetSelectedEntities()
{
    using namespace ui;
    std::vector<uint32_t> out;
    if (g_sel.kind == SelKind::Entity) {
        out.push_back(g_sel.entity);
        g_sel.kind = SelKind::None;   // «ворота» закрываются первыми; name не трогаем
    }
    return out;
}

void UI_ImGui::SetSelectedEntities(const std::vector<uint32_t>& entities)
{
    using namespace ui;
    if (entities.empty()) return;
    g_sel.entity = entities.front();   // порядок важен: entity ДО kind («ворота» открываются последними)
    g_sel.index  = -1;
    g_sel.kind   = SelKind::Entity;
}

// Фасад ImGui для игры (контракт — в заголовке): единственное место, где игровой ввод
// спрашивает редактор, не зная про ImGuiIO.
bool UI_ImGui::WantCaptureMouse()    { return ImGui::GetIO().WantCaptureMouse; }
bool UI_ImGui::WantCaptureKeyboard() { return ImGui::GetIO().WantCaptureKeyboard; }

void UI_ImGui::ProcessEvent(const SDL_Event& event)
{
    ImGui_ImplSDL3_ProcessEvent(&event);
}

void UI_ImGui::Iterate(EngineContext* ctx)
{
    ImGuizmo::BeginFrame();

    SetupDockspace();       // хост-докспейс + первичная раскладка панелей
    DrawHierarchy(ctx);     // слева
    DrawInspector(ctx);     // справа
    DrawAssetBrowser(ctx);  // снизу

    // Гизмо — ПОСЛЕ панелей: живёт не в окне, а поверх сцены (в прозрачной центральной ноде).
    DrawGizmo(ctx);
}

void UI_ImGui::SetupDockspace()
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    // id версионируем: смена строки заставляет пересобрать дефолтную раскладку поверх уже
    // сохранённого imgui.ini (старый node-id там просто не найдётся → ветка ниже сработает).
    ImGuiID dockspace_id = ImGui::GetID("EditorDockSpaceV2");

    // Первичная раскладка строится ОДИН раз и только если её не восстановил imgui.ini
    // (DockBuilderGetNode == null до первого DockSpaceOverViewport с этим id). Так дефолт
    // получаешь на чистом старте, а сохранённую раскладку не затираем.
    static bool checked = false;
    if (!checked) {
        checked = true;
        if (ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
            ImGui::DockBuilderRemoveNode(dockspace_id);
            ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspace_id, vp->Size);

            // Bottom откалываем ПЕРВЫМ от всего докспейса → он во всю ширину; Left/Right
            // отрезаем уже от верхнего остатка → колонки стоят НАД нижней панелью, не до края.
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

    // PassthruCentralNode: центральная нода прозрачна и не ловит мышь → сквозь неё видно
    // 3D-сцену и работает вращение камеры перетаскиванием (как раньше при !WantCaptureMouse).
    ImGui::DockSpaceOverViewport(dockspace_id, vp, ImGuiDockNodeFlags_PassthruCentralNode);
}
