#include "WindowTracker.h"
#include <dwmapi.h>
#include <utility>

bool WindowTracker::Initialize(HWND hwndSink, IconCache* cache, ChangeCallback onChange)
{
    hwndSink_    = hwndSink;
    iconCache_   = cache;
    onChange_    = std::move(onChange);
    shellHookMsg_ = RegisterWindowMessage(L"SHELLHOOK");

    RegisterShellHookWindow(hwndSink_);
    Seed();
    return true;
}

void WindowTracker::Shutdown()
{
    if (hwndSink_) {
        DeregisterShellHookWindow(hwndSink_);
        hwndSink_ = nullptr;
    }
}

void WindowTracker::Seed()
{
    struct Ctx { WindowTracker* self; };
    Ctx ctx{ this };

    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        if (c->self->ShouldTrack(hwnd))
            c->self->AddWindow(hwnd);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    UpdateActiveWindow();
}

bool WindowTracker::ShouldTrack(HWND hwnd) const
{
    if (!IsWindowVisible(hwnd)) return false;
    if (GetWindowTextLengthW(hwnd) == 0) return false;
    if (hwnd == hwndSink_) return false;

    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return false;
    if (!(exStyle & WS_EX_APPWINDOW) && GetWindow(hwnd, GW_OWNER) != nullptr) return false;

    // Cloaked windows are hidden by the system (virtual desktop, UWP background processes)
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
        return false;

    // Zero-size windows are placeholders / background handles
    RECT rc = {};
    GetWindowRect(hwnd, &rc);
    if (rc.right <= rc.left || rc.bottom <= rc.top) return false;

    // Block known system/shell window classes
    wchar_t cls[256] = {};
    GetClassNameW(hwnd, cls, 256);

    static constexpr const wchar_t* kBlocked[] = {
        L"Progman",                          // desktop
        L"WorkerW",                          // desktop worker
        L"Shell_TrayWnd",                    // Windows taskbar
        L"Shell_SecondaryTrayWnd",           // multi-monitor taskbar
        L"Windows.UI.Core.CoreWindow",       // UWP core (input experience, etc.)
        L"ForegroundStaging",                // foreground transition staging
        L"Shell_InputSwitchTopLevelWindow",  // touch keyboard / input switcher
        L"MSCTFIME UI",                      // IME candidate window
        L"tooltips_class32",                 // tooltips
        L"SysShadow",                        // DWM drop-shadow helper
        L"DWM Thumbnail Placeholder",        // DWM thumbnail
        L"EdgeUiInputTopWndClass",           // Edge swipe UI
        L"NativeHWNDHost",                   // WinRT XAML host (background)
        L"Windows.UI.Composition.DesktopWindowContentBridge", // composition bridge
    };
    for (const wchar_t* blocked : kBlocked)
        if (wcscmp(cls, blocked) == 0) return false;

    return true;
}

int WindowTracker::FindByHwnd(HWND hwnd) const
{
    for (int i = 0; std::cmp_less(i, buttons_.size()); ++i)
        if (buttons_[i].hwnd == hwnd) return i;
    return -1;
}

std::wstring WindowTracker::GetWindowTitle(HWND hwnd) const
{
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return {};
    std::wstring title(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(hwnd, title.data(), len + 1);
    title.resize(static_cast<size_t>(len));
    return title;
}

bool WindowTracker::AddWindowInternal(HWND hwnd)
{
    if (FindByHwnd(hwnd) >= 0) return false;

    TaskButton btn;
    btn.hwnd  = hwnd;
    btn.title = GetWindowTitle(hwnd);
    btn.icon  = iconCache_ ? iconCache_->GetIcon(hwnd, kIconSize) : nullptr;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid) {
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hProc) {
            wchar_t path[MAX_PATH] = {};
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameW(hProc, 0, path, &size))
                btn.exePath = path;
            CloseHandle(hProc);
        }
    }

    buttons_.push_back(btn);
    return true;
}

void WindowTracker::AddWindow(HWND hwnd)
{
    if (AddWindowInternal(hwnd) && onChange_) onChange_();
}

void WindowTracker::RemoveWindow(HWND hwnd)
{
    int idx = FindByHwnd(hwnd);
    if (idx < 0) return;

    if (iconCache_) iconCache_->Evict(hwnd);
    buttons_.erase(buttons_.begin() + idx);
    staleTicks_.erase(hwnd);
    if (onChange_) onChange_();
}

void WindowTracker::UpdateActiveWindow()
{
    HWND fg = GetForegroundWindow();
    // Only update when fg is a tracked window.  If it's an untracked companion
    // window (WinUI3 input-sink, ApplicationFrameWindow inner app, etc.) the
    // shell-hook already set isActive correctly — don't clobber it.
    bool fgTracked = false;
    for (const auto& b : buttons_)
        if (b.hwnd == fg) { fgTracked = true; break; }
    if (!fgTracked) return;

    bool changed = false;
    for (auto& btn : buttons_) {
        bool wasActive = btn.isActive;
        btn.isActive = (btn.hwnd == fg);
        if (btn.isActive != wasActive) changed = true;
    }
    if (changed && onChange_) onChange_();
}

void WindowTracker::RefreshTitle(HWND hwnd)
{
    int idx = FindByHwnd(hwnd);
    if (idx < 0) return;
    buttons_[idx].title = GetWindowTitle(hwnd);
    if (onChange_) onChange_();
}

void WindowTracker::RefreshIcon(HWND hwnd)
{
    int idx = FindByHwnd(hwnd);
    if (idx < 0) return;
    // Re-probe in the background; the current icon stays until the new one
    // resolves (via SetIcon) so a title change never blinks the icon.
    if (iconCache_) iconCache_->Refresh(hwnd, kIconSize);
    if (onChange_) onChange_();
}

void WindowTracker::SetIcon(HWND hwnd, HICON icon)
{
    int idx = FindByHwnd(hwnd);
    if (idx >= 0) buttons_[idx].icon = icon;
}

void WindowTracker::OnShellMessage(WPARAM wParam, LPARAM lParam)
{
    WPARAM code = wParam & 0x7FFF;
    HWND hwnd   = reinterpret_cast<HWND>(lParam);

    switch (code) {
    case HSHELL_WINDOWCREATED:
        if (hwnd && ShouldTrack(hwnd))
            AddWindow(hwnd);
        break;

    case HSHELL_WINDOWDESTROYED:
        // Delivered both for genuine destruction and, spuriously, while a window
        // animates (minimize/restore). Remove synchronously ONLY when the window is
        // truly gone (a real close is already !IsWindow by the time we process this
        // posted message) — that case can't flicker because the window won't return.
        // A window that still exists but looks non-trackable (hidden to tray, briefly
        // cloaked mid-animation, etc.) is left alone here and handled by Reconcile()
        // with a grace period, so a transient blip never removes a live button.
        if (!IsWindow(hwnd))
            RemoveWindow(hwnd);
        break;

    case HSHELL_WINDOWACTIVATED:
    case HSHELL_RUDEAPPACTIVATED:
        // Set isActive directly from the shell-hook HWND.  This is more reliable
        // than GetForegroundWindow(), which returns an untracked companion window
        // for ApplicationFrameWindow-hosted and WinUI3 apps (e.g. new Task Manager).
        if (hwnd) {
            bool changed = false;
            for (auto& b : buttons_) {
                bool was = b.isActive;
                b.isActive = (b.hwnd == hwnd);
                if (b.isActive != was) changed = true;
            }
            if (changed && onChange_) onChange_();
        }
        break;

    case HSHELL_REDRAW:
        if (hwnd) {
            if (ShouldTrack(hwnd) && FindByHwnd(hwnd) < 0)
                AddWindow(hwnd);
            else {
                RefreshTitle(hwnd);
                RefreshIcon(hwnd);
            }
        }
        break;

    default:
        break;
    }
}

void WindowTracker::Reconcile()
{
    bool changed = false;

    // 1. Add any currently trackable top-level window we don't have a button for.
    //    Catches windows whose HSHELL_WINDOWCREATED arrived before they had a title
    //    (e.g. a new Firefox window) and any create notification we missed entirely.
    struct Ctx { WindowTracker* self; bool* changed; };
    Ctx ctx{ this, &changed };
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        if (c->self->ShouldTrack(hwnd) && c->self->FindByHwnd(hwnd) < 0) {
            if (c->self->AddWindowInternal(hwnd))
                *c->changed = true;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    // 2. Drop buttons whose window is gone, or has stayed non-trackable (hidden to
    //    tray, moved to another virtual desktop, etc.) for kStaleThreshold consecutive
    //    ticks. Minimized windows are explicitly preserved: a minimized window fails
    //    ShouldTrack() (off-screen / degenerate rect, possible cloaking), but it must
    //    keep its taskbar button — that exact case was removing buttons mid-minimize
    //    and re-adding them on restore, which is what produced the disappear/flicker.
    for (int i = static_cast<int>(buttons_.size()) - 1; i >= 0; --i) {
        HWND h = buttons_[i].hwnd;
        bool drop = false;
        if (!IsWindow(h)) {
            drop = true;                         // truly gone — remove immediately
        } else if (IsIconic(h)) {
            staleTicks_.erase(h);                // minimized — always keep its button
        } else if (!ShouldTrack(h)) {
            if (++staleTicks_[h] >= kStaleThreshold)
                drop = true;                     // non-trackable long enough — remove
        } else {
            staleTicks_.erase(h);                // healthy again — reset its grace count
        }
        if (drop) {
            if (iconCache_) iconCache_->Evict(h);
            buttons_.erase(buttons_.begin() + i);
            staleTicks_.erase(h);
            changed = true;
        }
    }

    if (changed && onChange_) onChange_();
}
