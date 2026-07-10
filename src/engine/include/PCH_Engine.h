#pragma once
// PCH ВЕРХНЕГО слоя (таргет Engine и игры): общий PCH.h + imgui. Отдельным файлом,
// чтобы GPU-слой (EngineGpu) и ECS (EngineEcs) остались на чистом PCH.h — imgui в них
// по-прежнему не протекает (см. комментарий в PCH.h). imgui_internal.h сюда намеренно
// не входит: внутренний API подключают точечно только UI_ImGui.cpp/UI_Inspector.cpp.
#include "PCH.h"
#include "imgui.h"
