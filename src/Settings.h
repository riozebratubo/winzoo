#pragma once
#include <map>
#include <string>
#include <vector>
#include "Theme.h"

enum class TaskbarPosition         { Top, Bottom, Left, Right, Floating };
enum class AppMenuLayout           { List = 0, Grid = 1, Classic = 2, ClassicRounded = 3 };
enum class AppMenuFlattenMode      { None = 0, Submenus = 1, All = 2 };
enum class TaskbarMonitorMode      { AllMonitors = 0, Primary = 1 };
enum class MinimizedIndicatorType  { SmallRectangle = 0, DimButton = 1 };

// How winzoo makes ApplicationFrameWindow apps (Task Manager etc.) minimize toward
// winzoo's edge instead of Explorer's old one. See TaskbarRelocate.h.
//   None           — keep current behavior; don't touch Explorer's taskbar.
//   BeforeExplorer — relocate Explorer's taskbar to winzoo's edge (StuckRects3 +
//                    one explorer.exe restart). Robust; changes the real taskbar edge.
//
// ── Reserved: "After Explorer" method (would be enum value 2) ─────────────────
// Deliberately NOT defined / NOT shown in the settings UI because it is unimplemented.
// Intended behavior, for a future implementation:
//   AfterExplorer  — leave Explorer's taskbar where it is and instead intercept the
//                    taskbar-position query and return winzoo's rect, so AFW/UWP apps
//                    (Task Manager, Settings) read winzoo's edge as "the taskbar".
//                    The lighter alternative to Method B: no registry change and no
//                    explorer.exe restart. Likely implemented by extending the
//                    winzoo_com.dll hook already injected into Explorer's tray thread
//                    (see TaskbarProxy::Install) to answer ABM_GETTASKBARPOS / the
//                    internal Shell_TrayWnd position message with winzoo's rect.
//                    Caveat to verify first: that AFW apps actually resolve the taskbar
//                    via that query (rather than the registered appbar edge).
// To re-enable: add `AfterExplorer = 2` below, add its label to kTaskbarHookMethods[]
// in SettingsDialog.cpp, widen the load/readback/JSON clamps from 1 to 2, and handle
// it in App::Init().
enum class TaskbarHookMethod       { None = 0, BeforeExplorer = 1 };

struct Settings { // NOLINT(bugprone-exception-escape) — default ctor can throw (wstring inits); no noexcept contract exists
    TaskbarPosition position  = TaskbarPosition::Bottom;
    ThemePreset     theme     = ThemePreset::Default;
    int             thickness = 40;
    int             leftRightHeight  = 300;   // taskbar width in vertical (left/right) mode when titles are shown
    int             floatX           = 100;
    int             floatY           = 100;
    int             floatWidth       = 400;
    TaskbarHookMethod  taskbarHookMethod          = TaskbarHookMethod::BeforeExplorer;
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

    // Apps pinned by the user to the top of the Apps Menu (separate from taskbar pins).
    // Order is significant (drag-reorderable). Paths are .lnk/exe paths matching AppEntry.
    bool appMenuPinnedPerMonitor = true;  // default: per-monitor
    std::vector<std::wstring> appMenuPinnedPaths;  // global list
    std::map<std::wstring, std::vector<std::wstring>> appMenuPinnedPathsPerMonitor;

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
    bool appMenuSearchSystem           = true;  // show executables from PATH and Settings page links

    // App Menu Sidebar
    bool appMenuSidebarEnabled         = true;
    int  appMenuSidebarWidth           = 40;
    bool appMenuSidebarShowExplorer    = true;
    bool appMenuSidebarShowSettings    = true;
    bool appMenuSidebarShowPower       = true;

    // Classic (Vista/7) layout
    int  appMenuClassicPanelWidth        = 180;
    bool appMenuClassicShowDocuments     = true;
    bool appMenuClassicShowPictures      = true;
    bool appMenuClassicShowMusic         = true;
    bool appMenuClassicShowDownloads     = true;
    bool appMenuClassicShowRecentItems   = true;
    bool appMenuClassicShowThisPC        = true;
    bool appMenuClassicShowControlPanel  = true;
    bool appMenuClassicShowWinSettings   = true;
    bool appMenuClassicShowRun           = true;
    bool appMenuClassicShowShutDown      = true;

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
