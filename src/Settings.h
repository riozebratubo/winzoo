#pragma once
#include <map>
#include <string>
#include <vector>
#include "Theme.h"

enum class TaskbarPosition         { Top, Bottom, Left, Right, Floating };
enum class AppMenuLayout           { List = 0, Grid = 1 };
enum class AppMenuFlattenMode      { None = 0, Submenus = 1, All = 2 };
enum class TaskbarMonitorMode      { AllMonitors = 0, Primary = 1 };
enum class MinimizedIndicatorType  { SmallRectangle = 0, DimButton = 1 };

struct Settings { // NOLINT(bugprone-exception-escape) — default ctor can throw (wstring inits); no noexcept contract exists
    TaskbarPosition position  = TaskbarPosition::Bottom;
    ThemePreset     theme     = ThemePreset::Default;
    int             thickness = 48;
    int             leftRightHeight  = 300;   // taskbar width in vertical (left/right) mode when titles are shown
    int             floatX           = 100;
    int             floatY           = 100;
    int             floatWidth       = 400;
    TaskbarMonitorMode taskbarMonitorMode        = TaskbarMonitorMode::AllMonitors;
    bool               showAppMenuOnAllMonitors  = true;
    bool               showCurrentMonitorAppsOnly = true;
    int             maxButtonWidth    = 200;
    int             minButtonWidth    = 48;
    int             appButtonIconSize = 48;
    bool            middleClickClose  = true;
    bool            showRightClickGap = true;
    bool            showMinimizedIndicator = true;
    MinimizedIndicatorType minimizedIndicatorType = MinimizedIndicatorType::SmallRectangle;
    int             minimizedIndicatorW    = 10;
    int             minimizedIndicatorH    = 4;
    bool showStatusZone    = false;  // show volume / network / battery icons
    bool showLangIndicator = true;   // show current keyboard/input language (e.g. "EN", "JA")

    bool showTrayIcons     = true;   // show notification-area (system tray) icons
    bool showWinzooCustomIcons = false; // when true, shows winzoo's custom volume/network in the status zone
    bool trayIconFallbackExe = false; // fall back to app's exe icon when tray icon can't be retrieved
    bool showOverflowTrayIcons = true; // include hidden (overflow/chevron) tray icons
    int  trayIconSize      = 22;     // icon side length in logical pixels
    int  trayIconPadding   = 0;      // padding inside each icon slot (px each side)
    int  trayIconMargin    = 2;      // gap between adjacent icons (px)
    std::vector<std::wstring> trayIconOrder; // ordered list of icon keys (exeName|uID)

    bool showClock         = true;
    int             clockWidth        = 80;
    int             clockLineSpacing  = 0;
    std::wstring    clockTimeFormat   = L"$hh:$mm";
    std::wstring    clockDateFormat   = L"$dd/$mm/$yyyy";
    int             clockTimeFontSize = 9;
    int             clockDateFontSize = 8;
    COLORREF        clockTimeColor    = RGB(240, 240, 240);
    COLORREF        clockDateColor    = RGB(110, 110, 110);
    bool pinnedAppsAsButtonsWhenOpen = true;
    bool pinnedAppsPerMonitor        = true;
    std::vector<std::wstring> pinnedExePaths;
    // Per-monitor pinned paths, keyed by display name (e.g. "DISPLAY1").
    // Used when pinnedAppsPerMonitor is true.
    std::map<std::wstring, std::vector<std::wstring>> pinnedExePathsPerMonitor;

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

    // App Menu Search
    bool appMenuSearchEnabled          = true;
    bool appMenuSearchFuzzy            = true;

    // App Menu Sidebar
    bool appMenuSidebarEnabled         = true;
    int  appMenuSidebarWidth           = 40;
    bool appMenuSidebarShowExplorer    = true;
    bool appMenuSidebarShowSettings    = true;
    bool appMenuSidebarShowPower       = true;

    int      buttonOutlineRadius      = 0;    // corner radius of task button outline in logical px
    bool     showPinnedAppsAsButtons  = false; // draw outline + background for pinned-but-not-running buttons

    bool     showSeparators   = false;
    COLORREF separatorColor   = RGB(70, 70, 70);

    bool     showProgressBars         = true;
    bool     progressBarUseThemeColor = true;
    COLORREF progressBarColor         = RGB(0, 84, 153);
    int      progressBarHeight        = 3;   // logical px, range 1–10

    bool showTitlesOnVertical = false;   // show app titles in left/right position
    bool openAppsOnSameMonitor = false;  // open launched apps/dialogs on the taskbar's monitor

    // Visual — custom taskbar color override
    bool     useCustomTaskbarColor = false;
    COLORREF customTaskbarColor    = RGB(30, 30, 30);

    // Settings dialog geometry (0 = not yet saved, use default centering)
    int settingsDlgX = 0;
    int settingsDlgY = 0;
    int settingsDlgW = 0;  // window width in pixels
    int settingsDlgH = 0;  // window height in pixels
};
