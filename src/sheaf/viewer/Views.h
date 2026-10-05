#pragma once
// Окно вьювера на ImGui: список таблиц, сетка выбранной, панель выбранного объекта.
#include "Model.h"

struct Selection {
    int      table  = -1;
    uint32_t row    = 0;
    int      column = -1;   // индекс в ViewTable::columns; -1 — объект целиком
};

struct ViewerState {
    Model       model;
    int         current = -1;
    Selection   sel;
    int         scroll_to = -1;            // позиция в view, к которой прокрутить сетку в следующем кадре
    std::string error;
    bool        open_requested = false;    // кнопка «Открыть» / Ctrl+O — диалог показывает main
    int64_t     navigate_to = -1;          // сквозной номер объекта: переход в начале следующего кадра,
                                           // а не посреди обхода view, который он пересобирает

    int  filter_column = -1;               // ещё не применённый фильтр
    char filter_text[256] = {};
    bool filter_bad = false;
};

void OpenFile(ViewerState& s, const std::string& path);
void DrawViewer(ViewerState& s);
