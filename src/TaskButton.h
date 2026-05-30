#pragma once
#include <windows.h>
#include <string>
#include "Theme.h"
#include "Dpi.h"

struct MinimizedIndicatorOptions {
    bool enabled = true;
    int  width   = 10;
    int  height  = 4;
};

struct TaskButton {
    HWND         hwnd     = nullptr;
    HICON        icon     = nullptr;
    std::wstring title;
    std::wstring exePath;
    RECT         rect     = {};
    bool         isPinned = false;
    bool         isActive = false;

    bool IsRunning() const { return hwnd != nullptr; }

    bool HitTest(POINT pt) const
    {
        return PtInRect(&rect, pt) != FALSE;
    }

    void Draw(HDC hdc, const ThemeColors& colors,
              bool hovered, bool pressed, bool isDragGhost, int dpi,
              const MinimizedIndicatorOptions& indicator = {}) const;
};
