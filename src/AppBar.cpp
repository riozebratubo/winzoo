#include "AppBar.h"
#include <algorithm>

UINT AppBar::EdgeForPosition(TaskbarPosition p)
{
    switch (p) {
    case TaskbarPosition::Top:   return ABE_TOP;
    case TaskbarPosition::Left:  return ABE_LEFT;
    case TaskbarPosition::Right: return ABE_RIGHT;
    default:                     return ABE_BOTTOM;
    }
}

RECT AppBar::MonitorRectForWindow() const
{
    HMONITOR hMon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfo(hMon, &mi);
    return mi.rcMonitor;
}

bool AppBar::Register(HWND hwnd, TaskbarPosition position, int thicknessPx)
{
    hwnd_        = hwnd;
    position_    = position;
    thicknessPx_ = thicknessPx;

    if (!callbackMsg_)
        callbackMsg_ = RegisterWindowMessage(L"WinzooAppBarCallback");

    if (position == TaskbarPosition::Floating) {
        // Floating mode: no shell reservation
        return true;
    }

    abd_                  = {};
    abd_.cbSize           = sizeof(abd_);
    abd_.hWnd             = hwnd;
    abd_.uCallbackMessage = callbackMsg_;
    abd_.uEdge            = EdgeForPosition(position);

    SHAppBarMessage(ABM_NEW, &abd_);
    registered_ = true;

    return SetPosition(position, thicknessPx);
}

bool AppBar::SetPosition(TaskbarPosition position, int thicknessPx)
{
    position_    = position;
    thicknessPx_ = thicknessPx;

    if (position == TaskbarPosition::Floating || !registered_)
        return false;

    RECT mon = MonitorRectForWindow();
    abd_.uEdge = EdgeForPosition(position);
    abd_.rc    = mon;

    switch (abd_.uEdge) {
    case ABE_BOTTOM: abd_.rc.top    = abd_.rc.bottom - thicknessPx; break;
    case ABE_TOP:    abd_.rc.bottom = abd_.rc.top    + thicknessPx; break;
    case ABE_LEFT:   abd_.rc.right  = abd_.rc.left   + thicknessPx; break;
    case ABE_RIGHT:  abd_.rc.left   = abd_.rc.right  - thicknessPx; break;
    }

    SHAppBarMessage(ABM_QUERYPOS, &abd_);
    SHAppBarMessage(ABM_SETPOS,   &abd_);
    reservedRect_ = abd_.rc;

    SetWindowPos(hwnd_, HWND_TOPMOST,
                 reservedRect_.left, reservedRect_.top,
                 reservedRect_.right  - reservedRect_.left,
                 reservedRect_.bottom - reservedRect_.top,
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);
    return true;
}

void AppBar::Unregister()
{
    if (!registered_) return;
    abd_.cbSize = sizeof(abd_);
    abd_.hWnd   = hwnd_;
    SHAppBarMessage(ABM_REMOVE, &abd_);
    registered_ = false;
}

void AppBar::OnCallback(WPARAM wParam, LPARAM lParam)
{
    switch (wParam) {
    case ABN_POSCHANGED:
        SetPosition(position_, thicknessPx_);
        break;
    case ABN_FULLSCREENAPP:
        if (hwnd_)
            ShowWindow(hwnd_, lParam ? SW_HIDE : SW_SHOW);
        break;
    default:
        break;
    }
}
