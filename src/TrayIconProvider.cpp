#include "TrayIconProvider.h"
#include <commctrl.h>
#include <psapi.h>

// Layout of a notification-area button's dwData field in Explorer's process (x64).
// This is an undocumented but well-known structure used by Explorer.
#pragma pack(push, 1)
struct TrayData {
    HWND hWnd;           // 8 bytes on x64
    UINT uID;            // 4 bytes
    UINT uCallbackMsg;   // 4 bytes
    DWORD dwUserPref;    // 4 bytes
    DWORD pad;           // 4 bytes
    HICON hIcon;         // 8 bytes on x64 (USER object handle, valid cross-process)
};
#pragma pack(pop)

// Helper: read a block from another process.
static bool ReadRemote(HANDLE hProc, LPCVOID remote, void* local, SIZE_T size)
{
    SIZE_T read = 0;
    return ReadProcessMemory(hProc, remote, local, size, &read) && read == size;
}

// Helper: get tooltip text for button index from the toolbar's tooltip control.
static std::wstring GetButtonTooltip(HANDLE hProc, HWND hToolbar, int idx, LPVOID pShared)
{
    DWORD_PTR ttResult = 0;
    if (!SendMessageTimeoutW(hToolbar, TB_GETTOOLTIPS, 0, 0,
                             SMTO_ABORTIFHUNG, 500, &ttResult) || !ttResult)
        return {};
    HWND hTT = reinterpret_cast<HWND>(ttResult);

    // Write a TOOLINFO into shared memory and request the text.
    struct RemoteTI {
        UINT      cbSize;
        UINT      uFlags;
        HWND      hwnd;
        UINT_PTR  uId;
        RECT      rect;
        HINSTANCE hinst;
        LPWSTR    lpszText; // will be pointed at the text buffer inside pShared
        LPARAM    lParam;
        void*     lpReserved;
    };

    // We place TOOLINFO at pShared, and the text buffer right after it.
    const SIZE_T kTextOffset = sizeof(RemoteTI);
    const SIZE_T kTextBufLen = 256; // wchar_t count

    RemoteTI ti    = {};
    ti.cbSize      = sizeof(RemoteTI);
    ti.uFlags      = TTF_IDISHWND;
    ti.hwnd        = hToolbar;
    ti.uId         = static_cast<UINT_PTR>(idx);
    // Point lpszText at the memory just after the TOOLINFO struct in the remote allocation.
    ti.lpszText    = reinterpret_cast<LPWSTR>(
                         static_cast<char*>(pShared) + kTextOffset);

    BYTE* pTIRemote = static_cast<BYTE*>(pShared);
    if (!WriteProcessMemory(hProc, pTIRemote, &ti, sizeof(ti), nullptr))
        return {};

    // Zero out the text area.
    std::vector<wchar_t> zeroBuf(kTextBufLen, L'\0');
    WriteProcessMemory(hProc, pTIRemote + kTextOffset, zeroBuf.data(),
                       kTextBufLen * sizeof(wchar_t), nullptr);

    SendMessageTimeoutW(hTT, TTM_GETTEXT, kTextBufLen,
                        reinterpret_cast<LPARAM>(pTIRemote),
                        SMTO_ABORTIFHUNG, 500, nullptr);

    // Read back the text.
    std::vector<wchar_t> buf(kTextBufLen, L'\0');
    SIZE_T read = 0;
    ReadProcessMemory(hProc, pTIRemote + kTextOffset, buf.data(),
                      kTextBufLen * sizeof(wchar_t), &read);
    buf.back() = L'\0';
    return std::wstring(buf.data());
}

std::vector<TrayIconEntry> EnumerateTrayIcons(int /*iconSizePx*/)
{
    // Locate the notification area toolbar inside Explorer.
    // Windows 10 hierarchy: Shell_TrayWnd → TrayNotifyWnd → SysPager → ToolbarWindow32
    // Windows 11 hierarchy: Shell_TrayWnd → TrayNotifyWnd → ToolbarWindow32 (no SysPager)
    HWND hTray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!hTray) return {};

    HWND hNotify = FindWindowExW(hTray, nullptr, L"TrayNotifyWnd", nullptr);
    if (!hNotify) return {};

    // Try Windows 10 path (SysPager wrapper) first, then Windows 11 direct child.
    HWND hToolbar = nullptr;
    {
        HWND hPager = FindWindowExW(hNotify, nullptr, L"SysPager", nullptr);
        if (hPager)
            hToolbar = FindWindowExW(hPager, nullptr, L"ToolbarWindow32", nullptr);
    }
    if (!hToolbar)
        hToolbar = FindWindowExW(hNotify, nullptr, L"ToolbarWindow32", nullptr);
    if (!hToolbar) return {};

    DWORD_PTR btnCountResult = 0;
    if (!SendMessageTimeoutW(hToolbar, TB_BUTTONCOUNT, 0, 0,
                             SMTO_ABORTIFHUNG, 500, &btnCountResult))
        return {};
    int nButtons = static_cast<int>(btnCountResult);
    if (nButtons <= 0) return {};

    // Open Explorer's process for VM operations.
    DWORD explorerPid = 0;
    GetWindowThreadProcessId(hToolbar, &explorerPid);
    if (!explorerPid) return {};

    HANDLE hProc = OpenProcess(
        PROCESS_VM_READ | PROCESS_VM_OPERATION | PROCESS_VM_WRITE, FALSE, explorerPid);
    if (!hProc) return {};

    // Allocate a shared block in Explorer's address space large enough for one
    // TBBUTTON and a TOOLINFO + text buffer.
    const SIZE_T kSharedSize = sizeof(TBBUTTON) + sizeof(TrayData) + 4096;
    LPVOID pShared = VirtualAllocEx(hProc, nullptr, kSharedSize,
                                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pShared) {
        CloseHandle(hProc);
        return {};
    }

    std::vector<TrayIconEntry> result;

    for (int i = 0; i < nButtons; ++i) {
        // Ask Explorer to write the TBBUTTON into shared memory.
        SendMessageTimeoutW(hToolbar, TB_GETBUTTON, static_cast<WPARAM>(i),
                            reinterpret_cast<LPARAM>(pShared),
                            SMTO_ABORTIFHUNG, 500, nullptr);

        TBBUTTON btn = {};
        if (!ReadRemote(hProc, pShared, &btn, sizeof(btn))) continue;

        // Skip hidden buttons.
        if (btn.fsState & TBSTATE_HIDDEN) continue;

        TrayIconEntry entry = {};
        entry.uID           = static_cast<UINT>(btn.idCommand);

        // Read the TRAYDATA from Explorer's process via btn.dwData (a remote pointer).
        TrayData td = {};
        if (btn.dwData) {
            if (ReadRemote(hProc, reinterpret_cast<LPCVOID>(btn.dwData), &td, sizeof(td))) {
                entry.hWnd         = td.hWnd;
                entry.uCallbackMsg = td.uCallbackMsg;
                // Override uID with the one from TRAYDATA (more reliable).
                entry.uID          = td.uID;
            }
        }

        // Copy the tray icon from Explorer's TRAYDATA. HICON is a USER object
        // handle valid across processes; the returned icon is owned by us.
        if (td.hIcon)
            entry.hIcon = CopyIcon(td.hIcon);

        // Skip entries where we couldn't obtain a valid icon.
        if (!entry.hIcon) continue;

        // Tooltip.
        entry.tooltip = GetButtonTooltip(hProc, hToolbar, i, pShared);

        // Exe basename of the icon's owner process.
        if (entry.hWnd) {
            DWORD ownerPid = 0;
            GetWindowThreadProcessId(entry.hWnd, &ownerPid);
            if (ownerPid) {
                HANDLE hOwner = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                            FALSE, ownerPid);
                if (hOwner) {
                    wchar_t path[MAX_PATH] = {};
                    DWORD sz = MAX_PATH;
                    if (QueryFullProcessImageNameW(hOwner, 0, path, &sz)) {
                        std::wstring fullPath(path);
                        auto slash = fullPath.rfind(L'\\');
                        entry.exeName = (slash != std::wstring::npos)
                                        ? fullPath.substr(slash + 1) : fullPath;
                    }
                    CloseHandle(hOwner);
                }
            }
        }

        entry.orderKey = entry.exeName + L"|" + std::to_wstring(entry.uID);

        result.push_back(std::move(entry));
    }

    VirtualFreeEx(hProc, pShared, 0, MEM_RELEASE);
    CloseHandle(hProc);
    return result;
}
