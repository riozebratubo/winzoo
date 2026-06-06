#include "TaskbarRelocate.h"
#include <windows.h>
#include <shellapi.h>
#include <string>

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
