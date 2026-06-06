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

// Re-fit windows that are maximized on `mon` to `work`. We change the work area without
// SPIF_SENDCHANGE (broadcasting re-triggers the shell into re-stacking its own taskbar
// strip), so USER does not reposition maximized windows for us — they stay sized to the
// old, larger reservation, leaving a gap above the bar. Resize them ourselves, targeted
// to this monitor, so no global broadcast is involved.
static void RefitMaximizedWindows(HMONITOR mon, const RECT& work)
{
    struct Ctx { HMONITOR mon; RECT work; } ctx{ mon, work };
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        if (IsWindowVisible(hwnd) && IsZoomed(hwnd) &&
            MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) == c->mon) {
            SetWindowPos(hwnd, nullptr, c->work.left, c->work.top,
                         c->work.right - c->work.left, c->work.bottom - c->work.top,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
}

bool AppBar::SetPosition(TaskbarPosition position, int thicknessPx)
{
    position_    = position;
    thicknessPx_ = thicknessPx;

    if (position == TaskbarPosition::Floating || !registered_)
        return false;

    RECT mon  = MonitorRectForWindow();
    UINT edge = EdgeForPosition(position);

    // The strip we want to reserve on this monitor's edge.
    RECT strip = mon;
    StripForEdge(strip, edge, thicknessPx);

    // The appbar registration (ABM_SETPOS) is the part that must stay idempotent: the
    // shell sends ABN_POSCHANGED whenever ANY appbar moves, and re-issuing ABM_SETPOS
    // re-notifies others, which can feed an endless reassertion loop (the trembling).
    // So only touch the appbar + window size when OUR strip actually changed. The
    // work-area correction below, however, runs every call (see why there).
    bool stripChanged = !(haveApplied_ && edge == abd_.uEdge && EqualRect(&strip, &lastStrip_));

    if (stripChanged) {
        abd_.uEdge = edge;
        abd_.rc    = strip;
        SHAppBarMessage(ABM_QUERYPOS, &abd_);
        // ABM_QUERYPOS may have nudged the rect to avoid other appbars; re-clamp to our
        // exact thickness on the chosen edge so the bar size stays stable.
        StripForEdge(abd_.rc, edge, thicknessPx);
        SHAppBarMessage(ABM_SETPOS, &abd_);
        reservedRect_ = abd_.rc;

        SetWindowPos(hwnd_, HWND_TOPMOST,
                     reservedRect_.left, reservedRect_.top,
                     reservedRect_.right  - reservedRect_.left,
                     reservedRect_.bottom - reservedRect_.top,
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);

        lastStrip_   = strip;
        haveApplied_ = true;
    }

    // Force THIS monitor's work area to reserve ONLY our strip. Windows 11's taskbar
    // keeps a work-area reservation (~48px) even after SW_HIDE/ABM_REMOVE — it isn't a
    // classic appbar, so we can't remove it the normal way — and our own reservation
    // stacks under it (measured: 60px Explorer + 50px winzoo = 110px on the primary).
    // SPI_SETWORKAREA targets the monitor containing the rect, so this works per-monitor;
    // we read the live work area via GetMonitorInfo (SPI_GETWORKAREA only reports the
    // primary). Run on EVERY call (not gated by the idempotency check) so we re-correct
    // after the shell re-stacks, and WITHOUT SPIF_SENDCHANGE — broadcasting is exactly
    // what nudges the shell into re-reserving its strip, which created the stacking loop.
    if (!adjustingWorkArea_) {
        RECT desiredWA = mon;
        CarveWorkArea(desiredWA, edge, thicknessPx);
        MONITORINFO mi = { sizeof(mi) };
        HMONITOR hMon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY);
        if (GetMonitorInfo(hMon, &mi) && !EqualRect(&mi.rcWork, &desiredWA)) {
            adjustingWorkArea_ = true;
            SystemParametersInfo(SPI_SETWORKAREA, 0, &desiredWA, 0);
            // The silent SPI_SETWORKAREA above won't re-fit already-maximized windows;
            // do it ourselves so they fill the corrected area instead of staying short.
            RefitMaximizedWindows(hMon, desiredWA);
            adjustingWorkArea_ = false;
        }
    }

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
