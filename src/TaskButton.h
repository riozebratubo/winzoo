#pragma once
#include <windows.h>
#include <string>
#include "Theme.h"
#include "Dpi.h"
#include "Settings.h"

struct MinimizedIndicatorOptions {
    bool enabled = true;
    MinimizedIndicatorType type = MinimizedIndicatorType::SmallRectangle;
    int  width   = 10;
    int  height  = 4;
    int  dim     = 60;  // 0–100, % dimmed (higher = darker)
};

// TBPFLAG values (from shobjidl.h)
static constexpr int kTBPF_NOPROGRESS    = 0x0;
static constexpr int kTBPF_INDETERMINATE = 0x1;
static constexpr int kTBPF_NORMAL        = 0x2;
static constexpr int kTBPF_ERROR         = 0x4;
static constexpr int kTBPF_PAUSED        = 0x8;

struct ProgressInfo {
    int   state            = 0;    // kTBPF_* flag
    int   percent          = 0;    // 0–100
    DWORD lastProgressTick = 0;    // GetTickCount() at last relay; 0 = never received
};

struct ProgressBarOptions {
    bool     enabled = true;
    COLORREF color   = RGB(0, 84, 153);
    int      height  = 3;   // logical px
};

struct TaskButton {
    HWND         hwnd     = nullptr;
    HICON        icon     = nullptr;
    std::wstring title;
    std::wstring exePath;
    RECT         rect     = {};
    bool         isPinned   = false;
    bool         isActive   = false;
    bool         iconOnly   = false;  // draw only icon, centered (no text)
    int          iconDrawSz = 16;     // logical icon draw size in px
    HMONITOR     lastKnownMonitor = nullptr;  // monitor while non-minimized; reused when minimized
    POINT        minTarget = { 0x7FFFFFFF, 0x7FFFFFFF };  // last ptMinPosition we wrote (sentinel = none)
    ProgressInfo progress;

    bool IsRunning() const { return hwnd != nullptr; }

    bool HitTest(POINT pt) const
    {
        return PtInRect(&rect, pt) != FALSE;
    }

    void Draw(HDC hdc, const ThemeColors& colors,
              bool hovered, bool pressed, bool isDragGhost, int dpi,
              const MinimizedIndicatorOptions& indicator = {},
              const ProgressBarOptions& progressBar = {},
              int borderRadius = 0,
              bool showPinnedAsButtons = false) const;
};
