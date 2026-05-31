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

void WindowTracker::AddWindow(HWND hwnd)
{
    if (FindByHwnd(hwnd) >= 0) return;

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
    if (onChange_) onChange_();
}

void WindowTracker::RemoveWindow(HWND hwnd)
{
    int idx = FindByHwnd(hwnd);
    if (idx < 0) return;

    if (iconCache_) iconCache_->Evict(hwnd);
    buttons_.erase(buttons_.begin() + idx);
    if (onChange_) onChange_();
}

void WindowTracker::UpdateActiveWindow()
{
    HWND fg = GetForegroundWindow();
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
        RemoveWindow(hwnd);
        break;

    case HSHELL_WINDOWACTIVATED:
    case HSHELL_RUDEAPPACTIVATED:
        UpdateActiveWindow();
        break;

    case HSHELL_REDRAW:
        if (hwnd) {
            if (ShouldTrack(hwnd) && FindByHwnd(hwnd) < 0)
                AddWindow(hwnd);
            else
                RefreshTitle(hwnd);
        }
        break;

    default:
        break;
    }
}
