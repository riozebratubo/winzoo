#pragma once
#include <string>
#include <vector>
#include "Theme.h"

enum class TaskbarPosition { Top, Bottom, Left, Right, Floating };

struct Settings {
    TaskbarPosition position  = TaskbarPosition::Bottom;
    ThemePreset     theme     = ThemePreset::Dark;
    int             thickness = 48;
    int             floatX           = 100;
    int             floatY           = 100;
    int             maxButtonWidth    = 200;
    int             minButtonWidth    = 48;
    bool            middleClickClose  = true;
    bool            showRightClickGap = true;
    bool            showClock         = true;
    int             clockWidth        = 80;
    int             clockLineSpacing  = 0;
    std::wstring    clockTimeFormat   = L"$hh:$mm";
    std::wstring    clockDateFormat   = L"$dd/$mm/$yyyy";
    int             clockTimeFontSize = 9;
    int             clockDateFontSize = 8;
    COLORREF        clockTimeColor    = RGB(240, 240, 240);
    COLORREF        clockDateColor    = RGB(110, 110, 110);
    std::vector<std::wstring> pinnedExePaths;
};
