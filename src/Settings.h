#pragma once
#include <string>
#include <vector>
#include "Theme.h"

enum class TaskbarPosition    { Top, Bottom, Left, Right, Floating };
enum class AppMenuLayout      { List = 0, Grid = 1 };
enum class AppMenuFlattenMode { None = 0, Submenus = 1, All = 2 };
enum class TaskbarMonitorMode { AllMonitors = 0, Primary = 1 };

struct Settings {
    TaskbarPosition position  = TaskbarPosition::Bottom;
    ThemePreset     theme     = ThemePreset::Dark;
    int             thickness = 48;
    int             floatX           = 100;
    int             floatY           = 100;
    TaskbarMonitorMode taskbarMonitorMode        = TaskbarMonitorMode::AllMonitors;
    bool               showAppMenuOnAllMonitors  = true;
    bool               showCurrentMonitorAppsOnly = true;
    int             maxButtonWidth    = 200;
    int             minButtonWidth    = 48;
    bool            middleClickClose  = true;
    bool            showRightClickGap = true;
    bool            showMinimizedIndicator = true;
    int             minimizedIndicatorW    = 10;
    int             minimizedIndicatorH    = 4;
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

    // App Menu
    AppMenuLayout appMenuLayout       = AppMenuLayout::List;
    int           appMenuWidth        = 280;
    int           appMenuMaxHeight    = 600;
    int           appMenuEntryHeight  = 36;
    int           appMenuGridCols     = 4;
    int           appMenuGridRows     = 5;
    int           appMenuListFontSize = 9;
    int           appMenuGridFontSize = 8;
    int           appMenuMargin       = 0;
    int           appMenuPadding      = 6;

    // App Menu Behavior
    AppMenuFlattenMode appMenuFlattenMode      = AppMenuFlattenMode::Submenus;

    // App Menu Sidebar
    bool appMenuSidebarEnabled         = true;
    int  appMenuSidebarWidth           = 40;
    bool appMenuSidebarShowExplorer    = true;
    bool appMenuSidebarShowSettings    = true;
    bool appMenuSidebarShowPower       = true;
};
