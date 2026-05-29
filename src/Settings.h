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
    bool            middleClickClose = true;
    std::vector<std::wstring> pinnedExePaths;
};
