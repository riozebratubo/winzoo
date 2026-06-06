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

RECT AppBar::MonitorRectForWindow(bool* isPrimary) const
{
    HMONITOR hMon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfo(hMon, &mi);
    if (isPrimary)
        *isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
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

    // Force the next SetPosition to actually re-issue ABM_SETPOS: ABM_NEW resets the
    // shell's reservation, so the idempotency guard must not short-circuit it.
    haveApplied_ = false;
    lastStrip_   = {};

    return SetPosition(position, thicknessPx);
}

// Shrink a full-monitor rect down to a strip of `thickness` px on the given edge.
static void StripForEdge(RECT& r, UINT edge, int thickness)
{
    switch (edge) {
    case ABE_BOTTOM: r.top    = r.bottom - thickness; break;
    case ABE_TOP:    r.bottom = r.top    + thickness; break;
    case ABE_LEFT:   r.right  = r.left   + thickness; break;
    case ABE_RIGHT:  r.left   = r.right  - thickness; break;
    default: break;
    }
}

// Carve a strip of `thickness` px out of a full-monitor work-area rect on the given edge.
static void CarveWorkArea(RECT& r, UINT edge, int thickness)
{
    switch (edge) {
    case ABE_BOTTOM: r.bottom -= thickness; break;
    case ABE_TOP:    r.top    += thickness; break;
    case ABE_LEFT:   r.left   += thickness; break;
    case ABE_RIGHT:  r.right  -= thickness; break;
    default: break;
    }
}

bool AppBar::SetPosition(TaskbarPosition position, int thicknessPx)
{
    position_    = position;
    thicknessPx_ = thicknessPx;

    if (position == TaskbarPosition::Floating || !registered_)
        return false;

    bool isPrimary = false;
    RECT mon  = MonitorRectForWindow(&isPrimary);
    UINT edge = EdgeForPosition(position);

    // The strip we want to reserve on this monitor's edge.
    RECT strip = mon;
    StripForEdge(strip, edge, thicknessPx);

    // Idempotency guard: if our own reserved strip is unchanged since the last apply,
    // do nothing. The shell sends ABN_POSCHANGED whenever ANY appbar moves — including
    // Explorer's hidden taskbar reasserting its edge. Without this guard we'd re-issue
    // ABM_SETPOS, which re-notifies Explorer, which reasserts again: the endless
    // two-appbar loop that makes windows tremble and resize on every focus change.
    if (haveApplied_ && edge == abd_.uEdge && EqualRect(&strip, &lastStrip_))
        return true;

    abd_.uEdge = edge;
    abd_.rc    = strip;
    SHAppBarMessage(ABM_QUERYPOS, &abd_);
    // ABM_QUERYPOS may have nudged the rect to avoid other appbars; re-clamp to our
    // exact thickness on the chosen edge so the bar size stays stable.
    StripForEdge(abd_.rc, edge, thicknessPx);
    SHAppBarMessage(ABM_SETPOS, &abd_);
    reservedRect_ = abd_.rc;

    // The shell includes Explorer's hidden taskbar in its work area calculation even
    // after SW_HIDE (SW_HIDE doesn't call ABM_REMOVE). Override the work area so only
    // our strip is reserved. SPI_SETWORKAREA acts on the primary monitor's work area,
    // so only the primary bar drives it; doing it from every monitor's bar would let
    // them clobber each other. We also skip it when the work area is already correct,
    // which (together with the idempotency guard) stops the SPIF_SENDCHANGE broadcast
    // from feeding the loop.
    if (isPrimary && !adjustingWorkArea_) {
        RECT desiredWA = mon;
        CarveWorkArea(desiredWA, edge, thicknessPx);
        RECT curWA = {};
        SystemParametersInfo(SPI_GETWORKAREA, 0, &curWA, 0);
        if (!EqualRect(&curWA, &desiredWA)) {
            adjustingWorkArea_ = true;
            SystemParametersInfo(SPI_SETWORKAREA, 0, &desiredWA, SPIF_SENDCHANGE);
            adjustingWorkArea_ = false;
        }
    }

    SetWindowPos(hwnd_, HWND_TOPMOST,
                 reservedRect_.left, reservedRect_.top,
                 reservedRect_.right  - reservedRect_.left,
                 reservedRect_.bottom - reservedRect_.top,
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);

    lastStrip_   = strip;
    haveApplied_ = true;
    return true;
}

void AppBar::Unregister()
{
    if (!registered_) return;
    abd_.cbSize = sizeof(abd_);
    abd_.hWnd   = hwnd_;
    SHAppBarMessage(ABM_REMOVE, &abd_);
    registered_  = false;
    haveApplied_ = false;
    lastStrip_   = {};
}

void AppBar::OnCallback(WPARAM wParam, LPARAM lParam)
{
    switch (wParam) {
    case ABN_POSCHANGED:
        SetPosition(position_, thicknessPx_);
        break;
    case ABN_FULLSCREENAPP:
        // ABN_FULLSCREENAPP is system-wide, so only hide when the fullscreen app is
        // actually on this taskbar's monitor — otherwise a fullscreen window on one
        // monitor would blank the bars on all the others.
        if (hwnd_) {
            if (lParam) {
                HWND fg = GetForegroundWindow();
                HMONITOR fgMon = fg ? MonitorFromWindow(fg, MONITOR_DEFAULTTONULL) : nullptr;
                HMONITOR myMon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY);
                if (fgMon == myMon)
                    ShowWindow(hwnd_, SW_HIDE);
            } else {
                ShowWindow(hwnd_, SW_SHOW);
            }
        }
        break;
    default:
        break;
    }
}
