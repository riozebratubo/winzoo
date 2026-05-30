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
#include <vector>

class TaskbarWindow {
public:
    bool Create(HINSTANCE hInst, const Settings& settings);
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
    void ActivateButton(int idx);
    void ShowTaskButtonMenu(int idx, POINT ptScreen);
    void ShowBackgroundMenu(POINT ptScreen);
    void ShowAppMenu();
    void StartScanThread(bool isFirstScan);
    void StartIconLoadThread();
    RECT CalculateWindowRect() const;
    RECT GetStartBtnScreenRect() const;

    HWND            hwnd_           = nullptr;
    HINSTANCE       hInst_          = nullptr;
    Settings        settings_;
    ThemeColors     colors_         = {};
    int             dpi_            = 96;
    int             hoveredIdx_     = -1;
    int             hoveredScroll_  = 0;
    bool            hoveredStart_   = false;
    RECT            clockRect_      = {};
    bool            scrollNeeded_   = false;
    int             scrollOffset_   = 0;
    int             maxScrollOffset_= 0;
    RECT            scrollLeftRect_ = {};
    RECT            scrollRightRect_= {};
    RECT            startBtnRect_   = {};

    AppBar          appBar_;
    WindowTracker   tracker_;
    DragController  drag_;
    Renderer        renderer_;
    IconCache       iconCache_;

    std::vector<AppEntry> appEntries_;
    AppIconCache          appIconCache_;

    UINT            shellHookMsg_      = 0;
    UINT            taskbarCreatedMsg_ = 0;
    UINT            appBarCallbackMsg_ = 0;

    // Start-button toggle debounce: set true while menu is open to block
    // re-entrant ShowAppMenu() calls from the nested message loop;
    // menuLastClosedTick_ provides an extra 200ms guard after close.
    bool  menuOpen_             = false;
    DWORD menuLastClosedTick_   = 0;

    // Shutdown guard: set in WM_DESTROY so late WM_APP messages free heap and exit
    bool            shutdownPending_ = false;

    static constexpr UINT_PTR kTimerActiveWindow = 1;
    static constexpr UINT_PTR kTimerAppScanFirst = 2;
    static constexpr UINT_PTR kTimerAppScan      = 3;
    static constexpr UINT     kTimerIntervalMs   = 250;
    static constexpr UINT     kTimerAppScanMs    = 30000;

    // Custom WM_APP messages posted by background threads
    static constexpr UINT WM_APP_SCAN_DONE  = WM_APP + 1;
    static constexpr UINT WM_APP_ICONS_DONE = WM_APP + 2;
};
