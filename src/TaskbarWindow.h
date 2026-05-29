#pragma once
#include <windows.h>
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
    RECT CalculateWindowRect() const;

    HWND            hwnd_           = nullptr;
    HINSTANCE       hInst_          = nullptr;
    Settings        settings_;
    ThemeColors     colors_         = {};
    int             dpi_            = 96;
    int             hoveredIdx_     = -1;
    RECT            clockRect_      = {};

    AppBar          appBar_;
    WindowTracker   tracker_;
    DragController  drag_;
    Renderer        renderer_;
    IconCache       iconCache_;

    UINT            shellHookMsg_      = 0;
    UINT            taskbarCreatedMsg_ = 0;
    UINT            appBarCallbackMsg_ = 0;

    static constexpr UINT_PTR kTimerActiveWindow = 1;
    static constexpr UINT     kTimerIntervalMs   = 250;
};
