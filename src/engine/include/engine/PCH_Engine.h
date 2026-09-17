#pragma once
// PCH таргета Engine — единственного, кому доступен imgui (ImGui линкуется сюда PRIVATE).
// Отдельный файл нужен не ради скорости, а потому что общий PCH с imgui не собрался бы
// в EngineCore/Ecs/Gpu: у них нет его include-путей. imgui_internal.h сюда намеренно не
// входит: внутренний API подключают точечно только UI_ImGui.cpp/UI_Inspector.cpp.
#include "PCH.h"
#include "imgui.h"
