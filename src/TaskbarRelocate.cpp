#include "TaskbarRelocate.h"
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <algorithm>

static constexpr wchar_t kStuckKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StuckRects3";

// StuckRects3\Settings is a REG_BINARY blob describing the persisted taskbar state.
// The docked edge is the DWORD at byte offset 12: 0=left, 1=top, 2=right, 3=bottom.
// We touch only that one byte so all other taskbar settings (size, auto-hide, etc.)
// are preserved.
static constexpr size_t kEdgeOffset = 12;

static bool EdgeByteForPosition(TaskbarPosition pos, BYTE& outEdge)
{
    switch (pos) {
    case TaskbarPosition::Left:   outEdge = 0; return true;
    case TaskbarPosition::Top:    outEdge = 1; return true;
    case TaskbarPosition::Right:  outEdge = 2; return true;
    case TaskbarPosition::Bottom: outEdge = 3; return true;
    default: return false;  // Floating: no fixed edge to match
    }
}

// Returns Explorer's real Shell_TrayWnd (skips winzoo's own proxy window, if any).
static HWND FindExplorerTray()
{
    DWORD myPid = GetCurrentProcessId();
    for (HWND h = FindWindowW(L"Shell_TrayWnd", nullptr); h;
         h = FindWindowExW(nullptr, h, L"Shell_TrayWnd", nullptr)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid && pid != myPid) return h;
    }
    return nullptr;
}

static void RelaunchShell()
{
    wchar_t winDir[MAX_PATH] = {};
    if (GetWindowsDirectoryW(winDir, MAX_PATH)) {
        std::wstring exe = std::wstring(winDir) + L"\\explorer.exe";
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        if (CreateProcessW(exe.c_str(), nullptr, nullptr, nullptr, FALSE,
                           0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return;
        }
    }
    ShellExecuteW(nullptr, L"open", L"explorer.exe", nullptr, nullptr, SW_SHOWNORMAL);
}

// Infer which monitor edge a (visible) tray window is docked on from its rectangle.
static UINT EdgeFromTrayRect(HWND tray)
{
    RECT r{};
    if (!GetWindowRect(tray, &r)) return ABE_BOTTOM;
    HMONITOR hm = MonitorFromWindow(tray, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfo(hm, &mi)) return ABE_BOTTOM;
    RECT m = mi.rcMonitor;
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w >= h) // horizontal bar -> top or bottom
        return (r.top - m.top) <= (m.bottom - r.bottom) ? ABE_TOP : ABE_BOTTOM;
    // vertical bar -> left or right
    return (r.left - m.left) <= (m.right - r.right) ? ABE_LEFT : ABE_RIGHT;
}

void RemoveShellAppBarReservation(HWND tray)
{
    if (!tray) return;
    APPBARDATA abd{};
    abd.cbSize = sizeof(abd);
    abd.hWnd   = tray;
    // The shell tracks appbars by hWnd globally, so removing Explorer's tray strip from
    // another process frees the work area it was reserving. Harmless if it wasn't there.
    SHAppBarMessage(ABM_REMOVE, &abd);
}

void RestoreShellAppBarReservation(HWND tray)
{
    if (!tray) return;
    RECT r{};
    if (!GetWindowRect(tray, &r)) return;
    APPBARDATA abd{};
    abd.cbSize = sizeof(abd);
    abd.hWnd   = tray;
    abd.uEdge  = EdgeFromTrayRect(tray);
    abd.rc     = r;
    // Re-add the reservation so a visible taskbar reserves its strip again after winzoo
    // quits. ABM_NEW is a no-op (returns FALSE) if it's somehow still registered.
    SHAppBarMessage(ABM_NEW,    &abd);
    SHAppBarMessage(ABM_SETPOS, &abd);
}

void HideExplorerTaskbars()
{
    const DWORD myPid = GetCurrentProcessId();

    // Primary tray(s). Skip winzoo's own Shell_TrayWnd proxy (same PID) — only hide
    // Explorer's. Guard on visibility so repeated calls are cheap no-ops.
    for (HWND h = FindWindowExW(nullptr, nullptr, L"Shell_TrayWnd", nullptr); h;
         h = FindWindowExW(nullptr, h, L"Shell_TrayWnd", nullptr)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == myPid) continue;
        if (IsWindowVisible(h)) {
            ShowWindow(h, SW_HIDE);
            RemoveShellAppBarReservation(h);
        }
    }

    // Secondary trays (one per extra monitor). winzoo does not proxy this class, so any
    // window of it is Explorer's. Windows 11 re-creates/re-shows these on display and
    // work-area changes, which is why this must be callable repeatedly.
    for (HWND h = FindWindowExW(nullptr, nullptr, L"Shell_SecondaryTrayWnd", nullptr); h;
         h = FindWindowExW(nullptr, h, L"Shell_SecondaryTrayWnd", nullptr)) {
        if (IsWindowVisible(h)) {
            ShowWindow(h, SW_HIDE);
            RemoveShellAppBarReservation(h);
        }
    }
}

// Re-fit windows maximized on `mon` to `work` after a silent work-area change. Mirrors
// AppBar's helper: SPI_SETWORKAREA without SPIF_SENDCHANGE won't resize already-maximized
// windows, so we do it ourselves, targeted to this monitor (no global broadcast that would
// nudge the shell into re-reserving its strip).
static void RefitMaximizedWindowsOnMonitor(HMONITOR mon, const RECT& work)
{
    struct Ctx { HMONITOR mon; RECT work; } ctx{ mon, work };
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        if (IsWindowVisible(hwnd) && IsZoomed(hwnd) &&
            MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) == c->mon) {
            // Net-change guard: skip windows already at the target rect so redundant
            // reclaim passes don't re-resize a maximized window we've already fitted.
            RECT cur = {};
            if (GetWindowRect(hwnd, &cur) && EqualRect(&cur, &c->work))
                return TRUE;
            SetWindowPos(hwnd, nullptr, c->work.left, c->work.top,
                         c->work.right - c->work.left, c->work.bottom - c->work.top,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
}

void ReclaimUnoccupiedWorkAreas()
{
    // Monitors that currently host a winzoo bar — their AppBar manages the work area.
    std::vector<HMONITOR> occupied;
    for (HWND h = FindWindowExW(nullptr, nullptr, L"WinzooTaskbar", nullptr); h;
         h = FindWindowExW(nullptr, h, L"WinzooTaskbar", nullptr)) {
        if (HMONITOR m = MonitorFromWindow(h, MONITOR_DEFAULTTONULL))
            occupied.push_back(m);
    }

    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR hMon, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* occ = reinterpret_cast<std::vector<HMONITOR>*>(lp);
            if (std::find(occ->begin(), occ->end(), hMon) != occ->end())
                return TRUE;  // winzoo owns this monitor; leave its work area to the AppBar
            MONITORINFO mi{ sizeof(mi) };
            if (!GetMonitorInfo(hMon, &mi)) return TRUE;
            if (EqualRect(&mi.rcWork, &mi.rcMonitor)) return TRUE;  // already full, nothing reserved
            RECT full = mi.rcMonitor;
            // No SPIF_SENDCHANGE: broadcasting is what nudges the shell into re-reserving.
            SystemParametersInfoW(SPI_SETWORKAREA, 0, &full, 0);
            RefitMaximizedWindowsOnMonitor(hMon, full);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&occupied));
}

bool RelocateExplorerTaskbarToMatch(TaskbarPosition position)
{
    BYTE wantEdge = 0;
    if (!EdgeByteForPosition(position, wantEdge))
        return false;  // floating — nothing to relocate

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kStuckKey, 0,
                      KEY_READ | KEY_WRITE, &hKey) != ERROR_SUCCESS)
        return false;

    BYTE blob[256] = {};
    DWORD size = sizeof(blob), type = 0;
    LSTATUS rs = RegQueryValueExW(hKey, L"Settings", nullptr, &type, blob, &size);
    if (rs != ERROR_SUCCESS || type != REG_BINARY || size <= kEdgeOffset) {
        RegCloseKey(hKey);
        return false;
    }

    if (blob[kEdgeOffset] == wantEdge) {
        RegCloseKey(hKey);   // already on the matching edge — no restart needed
        return false;
    }

    blob[kEdgeOffset] = wantEdge;
    LSTATUS writeErr = RegSetValueExW(hKey, L"Settings", 0, REG_BINARY, blob, size);
    if (writeErr != ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return false;
    }
    RegCloseKey(hKey);

    // Explorer reads StuckRects3 only at shell startup, so it must restart to apply.
    HWND tray = FindExplorerTray();
    if (!tray) return false;

    DWORD explorerPid = 0;
    GetWindowThreadProcessId(tray, &explorerPid);

    // Graceful "exit Explorer" — the hidden taskbar shutdown command. Unlike a forced
    // kill it lets Explorer close cleanly and, crucially, does NOT auto-restart the
    // shell, so we relaunch it ourselves below.
    PostMessageW(tray, 0x5B4 /* WM_USER + 436 */, 0, 0);

    // Wait for the old shell process to exit so our relaunch becomes the shell rather
    // than just opening a folder window. If it refuses to exit (older command behavior),
    // we leave it alone instead of force-killing and lose nothing.
    bool exited = false;
    if (explorerPid) {
        HANDLE hProc = OpenProcess(SYNCHRONIZE, FALSE, explorerPid);
        if (hProc) {
            exited = (WaitForSingleObject(hProc, 8000) == WAIT_OBJECT_0);
            CloseHandle(hProc);
        }
    }
    if (!exited)
        return false;  // shell still alive; don't risk a duplicate explorer

    // The system may auto-relaunch the shell on some configurations; only relaunch
    // ourselves if no tray has reappeared, to avoid a stray Explorer window.
    for (int i = 0; i < 10 && !FindExplorerTray(); ++i)
        Sleep(200);
    if (!FindExplorerTray())
        RelaunchShell();

    return true;
}
