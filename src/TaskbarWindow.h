#pragma once
#include <windows.h>
#include <thread>
#include "Settings.h"
#include "Theme.h"
#include "AppBar.h"
#include "WindowTracker.h"
#include "DragController.h"
#include "Renderer.h"
#include "IconCache.h"
#include "PopupMenu.h"
#include "SettingsDialog.h"
#include "Dpi.h"
#include "AppEntry.h"
#include "AppScanner.h"
#include "AppIconCache.h"
#include "AppMenuWindow.h"
#include "SystemStatus.h"
#include "TrayIconProvider.h"
#include "TaskbarProxy.h"
#include <vector>

class TaskbarWindow {
public:
    bool Create(HINSTANCE hInst, const Settings& settings,
                HMONITOR hMonitor = nullptr, bool isPrimary = true);
    void SetMonitor(HMONITOR hMonitor, bool isPrimary);
    void Show();
    void Destroy();
    void ApplySettings(const Settings& s);

    HWND Hwnd() const { return hwnd_; }

private:
    static bool RegisterWndClass(HINSTANCE hInst);
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    LRESULT HandleMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    void LayoutButtons();
    int  HitTestButton(POINT pt) const;
    void ActivateButton(int combinedIdx);
    void ShowButtonMenu(int combinedIdx, POINT ptScreen);
    void ShowBackgroundMenu(POINT ptScreen);
    void ShowStatusIconMenu(int which, POINT ptScreen);  // 1=vol, 2=net, 3=bat
    void ShowAppMenu();
    void StartScanThread(bool isFirstScan);
    void StartIconLoadThread();
    void LaunchApp(const wchar_t* exe, const wchar_t* args = nullptr, int nShow = SW_SHOWNORMAL);
    RECT CalculateWindowRect() const;
    RECT GetStartBtnScreenRect() const;

    // Pinned buttons management
    void RebuildPinnedButtons();
    void UpdateMonitorDeviceName();

    // Tray icon management
    void RefreshTrayIcons();

    // Combined-index helpers (0..P-1 = pinned, P..P+T-1 = task)
    int             TotalCount()        const;
    bool            IsPinnedIdx(int i)  const;
    const TaskButton& GetButtonByIdx(int i) const;
    TaskButton&       GetMutableButtonByIdx(int i);

    // Returns the HWND of the first task button whose exePath matches (case-insensitive)
    HWND            FindHwndByExePath(const std::wstring& exePath) const;
    // Returns true if any task button has a matching exePath
    bool            IsExeRunning(const std::wstring& exePath) const;

    // Returns exe file name without extension (for pinned button titles)
    static std::wstring ExeBaseName(const std::wstring& exePath);

    // Pointer-sized and container fields first (8-byte aligned on 64-bit)
    HWND            hwnd_           = nullptr;
    HINSTANCE       hInst_          = nullptr;
    HMONITOR        hMonitor_       = nullptr;
    std::vector<TaskButton>        pinnedButtons_;   // rebuilt from settings_.pinnedExePaths
    std::vector<AppEntry>          appEntries_;
    std::vector<TrayIconEntry>     trayIcons_;       // owns the HICONs
    std::vector<RECT>              trayIconRects_;
    std::wstring    monitorDeviceName_;  // e.g. "DISPLAY1", stripped of "\\.\\" prefix
    ThemeColors     colors_         = {};
    Renderer        renderer_;
    IconCache       iconCache_;
    AppIconCache    appIconCache_;
    AppBar          appBar_;
    WindowTracker   tracker_;
    TaskbarProxy    proxy_;
    Settings        settings_;
    DragController  drag_;
    SystemStatusData statusData_    = {};
    POINT           trayDragPt_     = {}; // client coords

    // 4-byte aligned fields
    int             dpi_            = 96;
    int             hoveredIdx_     = -1;  // combined index
    int             hoveredScroll_  = 0;
    int             hoveredStatus_  = 0;   // 0=none 1=vol 2=net 3=bat
    int             scrollOffset_   = 0;
    int             maxScrollOffset_= 0;
    int             pinnedSepX_     = 0;   // x position of pinned/task zone separator
    UINT            shellHookMsg_      = 0;
    UINT            taskbarCreatedMsg_ = 0;
    UINT            appBarCallbackMsg_ = 0;
    UINT            progressRelayMsg_  = 0;
    DWORD           menuLastClosedTick_= 0;
    int             hoveredTrayIdx_ = -1;
    int             trayDragStart_  = -1;  // index pressed
    RECT            clockRect_      = {};
    RECT            statusZoneRect_ = {};
    RECT            volIconRect_    = {};
    RECT            netIconRect_    = {};
    RECT            batIconRect_    = {};
    RECT            scrollLeftRect_ = {};
    RECT            scrollRightRect_= {};
    RECT            startBtnRect_   = {};
    RECT            trayZoneRect_   = {};

    // 1-byte aligned bools last
    bool            isPrimary_      = true;
    bool            showStartButton_= true;
    bool            hoveredStart_   = false;
    bool            scrollNeeded_   = false;
    bool            menuOpen_       = false;
    bool            trayDragging_   = false;
    bool            shutdownPending_= false;

    static constexpr UINT_PTR kTimerActiveWindow = 1;
    static constexpr UINT_PTR kTimerAppScanFirst = 2;
    static constexpr UINT_PTR kTimerAppScan      = 3;
    static constexpr UINT_PTR kTimerStatus       = 4;
    static constexpr UINT_PTR kTimerTray         = 5;
    static constexpr UINT     kTimerIntervalMs   = 250;
    static constexpr UINT     kTimerAppScanMs    = 30000;
    static constexpr UINT     kTimerStatusMs     = 1000;
    static constexpr UINT     kTimerTrayMs       = 2000;

    // Custom WM_APP messages posted by background threads
    static constexpr UINT WM_APP_SCAN_DONE  = WM_APP + 1;
    static constexpr UINT WM_APP_ICONS_DONE = WM_APP + 2;
    static constexpr UINT WM_APP_SHOW_MENU  = WM_APP + 3;
};
