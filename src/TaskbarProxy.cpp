#include "TaskbarProxy.h"
#include "resource.h"

static constexpr wchar_t kInProcKey[] =
    L"Software\\Classes\\CLSID\\{56FDF344-FD6D-11d0-958A-006097C9A090}\\InProcServer32";
static constexpr wchar_t kClsidKey[] =
    L"Software\\Classes\\CLSID\\{56FDF344-FD6D-11d0-958A-006097C9A090}";

// ---------------------------------------------------------------------------

LRESULT CALLBACK TaskbarProxy::ProxyWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    // Relay progress messages (shell32's internal 0x04F3) to winzoo's taskbar windows.
    auto* self = reinterpret_cast<TaskbarProxy*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && self->relayMsg_ && msg == 0x04F3 && self->winzooHwnd_) {
        // Extract HWND (wParam) + state/percent from shell message and broadcast
        // via the registered WinzooProgress message.
        PostMessageW(self->winzooHwnd_, self->relayMsg_, wParam, lParam);
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------

bool TaskbarProxy::Install(HWND winzooHwnd) {
    relayMsg_    = RegisterWindowMessageW(L"WinzooProgress");
    if (!relayMsg_) return false;
    winzooHwnd_  = winzooHwnd;

    // --- Extract embedded DLL to a per-launch unique path under %LOCALAPPDATA% ---
    // Deliberately NOT next to the exe: that file is the build output, and once Explorer
    // injects it, the lock would block rebuilds. A unique name per launch also avoids
    // loading a stale copy that a previously force-killed session left mapped in Explorer.
    std::wstring dir;
    {
        wchar_t local[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH))
            dir = std::wstring(local) + L"\\winzoo";
        else {  // fallback: next to the exe
            wchar_t exePath[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            if (wchar_t* p = wcsrchr(exePath, L'\\')) *p = L'\0';
            dir = exePath;
        }
        CreateDirectoryW(dir.c_str(), nullptr);

        // Best-effort cleanup of stale copies from prior sessions (locked ones are skipped).
        WIN32_FIND_DATAW fd = {};
        HANDLE hFind = FindFirstFileW((dir + L"\\winzoo_com_*.dll").c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do { DeleteFileW((dir + L"\\" + fd.cFileName).c_str()); }
            while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }
    }
    dllPath_ = dir + L"\\winzoo_com_" + std::to_wstring(GetCurrentProcessId()) + L".dll";

    DeleteFileW(dllPath_.c_str());
    HRSRC hRes = FindResource(nullptr, MAKEINTRESOURCE(IDR_WINZOO_COM_DLL), RT_RCDATA);
    if (hRes) {
        HGLOBAL hGlob = LoadResource(nullptr, hRes);
        LPVOID  pData = LockResource(hGlob);
        DWORD   sz    = SizeofResource(nullptr, hRes);
        if (pData && sz) {
            HANDLE hf = CreateFileW(dllPath_.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hf != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(hf, pData, sz, &written, nullptr);
                CloseHandle(hf);
            }
        }
    }

    // --- COM DLL registration (HKCU override) ---
    const std::wstring& dllPath = dllPath_;

    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kInProcKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, nullptr, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(dllPath.c_str()),
                       static_cast<DWORD>((dllPath.size() + 1) * sizeof(wchar_t)));
        static const wchar_t kApartment[] = L"Apartment";
        RegSetValueExW(hKey, L"ThreadingModel", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(kApartment), sizeof(kApartment));
        RegCloseKey(hKey);
        registered_ = true;
    }

    // --- Proxy Shell_TrayWnd window ---
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = ProxyWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Shell_TrayWnd";
    if (RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        proxyHwnd_ = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            L"Shell_TrayWnd", nullptr, WS_POPUP,
            0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (proxyHwnd_) {
            SetWindowLongPtrW(proxyHwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            SetWindowPos(proxyHwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
    }

    // Sync both the proxy and Explorer's real Shell_TrayWnd to Winzoo's screen rect.
    {
        RECT rc = {};
        GetWindowRect(winzooHwnd_, &rc);
        UpdatePosition(rc);
    }

    // Hook Explorer's Shell_TrayWnd thread (progress + tray interception). This can
    // legitimately fail here when Explorer is mid-restart (Winzoo itself may relocate
    // and restart it at startup), so it is also retried from the TaskbarCreated handler.
    EnsureExplorerHook();

    return true;
}

// (Re)install the Explorer-thread hook. Idempotent per-Explorer-instance: if the same
// Explorer process is already hooked, it does nothing. On a fresh install it also
// re-broadcasts TaskbarCreated so apps re-issue Shell_NotifyIcon(NIM_ADD) AND
// ITaskbarList3 progress *after* our hook is live, giving a complete tray snapshot.
bool TaskbarProxy::EnsureExplorerHook() {
    DWORD myPid = GetCurrentProcessId();

    // Find Explorer's real Shell_TrayWnd (not our topmost proxy, not our own process).
    HWND explorerTray = nullptr;
    DWORD explorerPid = 0;
    for (HWND h = FindWindowW(L"Shell_TrayWnd", nullptr); h;
         h = FindWindowExW(nullptr, h, L"Shell_TrayWnd", nullptr)) {
        if (h == proxyHwnd_) continue;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid && pid != myPid) { explorerTray = h; explorerPid = pid; break; }
    }

    if (!explorerTray)
        return false;

    // Already hooked to this same Explorer instance — nothing to do.
    if (hookedExplorerPid_ == explorerPid && hHookDll_)
        return false;

    if (!hHookDll_) {
        hHookDll_ = LoadLibraryW(dllPath_.c_str());
        if (!hHookDll_) return false;
    }

    using FnInstall   = void (__stdcall *)(HWND);
    using FnUninstall = void (__stdcall *)();
    auto fnInstall   = reinterpret_cast<FnInstall>(
        GetProcAddress(hHookDll_, "WinzooCom_InstallHook"));
    auto fnUninstall = reinterpret_cast<FnUninstall>(
        GetProcAddress(hHookDll_, "WinzooCom_UninstallHook"));
    if (!fnInstall) return false;

    // Explorer changed identity (restart): drop the stale hook handles first.
    if (hookedExplorerPid_ && hookedExplorerPid_ != explorerPid && fnUninstall)
        fnUninstall();

    fnInstall(explorerTray);
    hookedExplorerPid_ = explorerPid;

    // Re-broadcast so apps repopulate now that the hook is live.
    UINT taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    if (taskbarCreatedMsg)
        PostMessageW(HWND_BROADCAST, taskbarCreatedMsg, 0, 0);

    return true;
}

void TaskbarProxy::UpdatePosition(RECT rc) {
    if (updatingPosition_) return;
    updatingPosition_ = true;

    if (proxyHwnd_)
        SetWindowPos(proxyHwnd_, nullptr,
                     rc.left, rc.top,
                     rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOACTIVATE | SWP_NOZORDER);

    // DWM holds a direct cached HWND reference to Explorer's real Shell_TrayWnd
    // (obtained at system startup) and reads its screen rect to determine where to
    // animate minimizing windows. Moving the hidden tray window to match Winzoo's
    // position redirects those animations to the correct edge.
    //
    // Re-find whenever the cached handle is stale — crucially after winzoo restarts
    // Explorer (BeforeExplorer), where the cached handle points at the destroyed old
    // tray and the new Shell_TrayWnd would otherwise never get moved, leaving every
    // window minimizing toward Explorer's default edge instead of winzoo's.
    if (!explorerTray_ || !IsWindow(explorerTray_)) {
        explorerTray_ = nullptr;
        for (HWND h = FindWindowW(L"Shell_TrayWnd", nullptr); h;
             h = FindWindowExW(nullptr, h, L"Shell_TrayWnd", nullptr)) {
            if (h == proxyHwnd_) continue;
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid != GetCurrentProcessId()) { explorerTray_ = h; break; }
        }
    }
    if (explorerTray_) {
        SetWindowPos(explorerTray_, nullptr,
                     rc.left, rc.top,
                     rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSENDCHANGING);
    }

    updatingPosition_ = false;
}

void TaskbarProxy::Uninstall() {
    if (hHookDll_) {
        using FnUninstall = void (__stdcall *)();
        auto fnUninstall = reinterpret_cast<FnUninstall>(
            GetProcAddress(hHookDll_, "WinzooCom_UninstallHook"));
        if (fnUninstall) fnUninstall();
        FreeLibrary(hHookDll_);
        hHookDll_ = nullptr;
    }
    hookedExplorerPid_ = 0;

    if (proxyHwnd_) {
        DestroyWindow(proxyHwnd_);
        proxyHwnd_ = nullptr;
    }
    winzooHwnd_ = nullptr;

    if (!registered_) return;
    RegDeleteTreeW(HKEY_CURRENT_USER, kClsidKey);
    registered_ = false;
    relayMsg_   = 0;

    if (!dllPath_.empty()) {
        DeleteFileW(dllPath_.c_str());
        dllPath_.clear();
    }
}
