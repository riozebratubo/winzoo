#pragma once
#include <windows.h>
#include <functional>

// Installs a WH_CALLWNDPROC global hook (via winzoo_hook.dll) to intercept
// ITaskbarList3 progress messages sent by apps to Shell_TrayWnd, and relays
// them to this process via a registered "WinzooProgress" window message.
class TaskbarProxy {
public:
    // Returns false if Shell_TrayWnd is not found or hook DLL cannot be loaded.
    bool Install(HWND winzooHwnd);
    void Uninstall();

    // The registered relay message ID (0 until Install() succeeds).
    UINT RelayMsg() const { return relayMsg_; }

    // Decode a relay message: appHwnd=wParam, state=LOWORD(lParam), percent=HIWORD(lParam)
    static HWND  DecodeHwnd   (WPARAM wp) { return reinterpret_cast<HWND>(wp); }
    static int   DecodeState  (LPARAM lp) { return static_cast<int>(LOWORD(lp)); }
    static int   DecodePercent(LPARAM lp) { return static_cast<int>(HIWORD(lp)); }

    ~TaskbarProxy() { Uninstall(); }

private:
    HMODULE hookDll_  = nullptr;
    UINT    relayMsg_ = 0;
};
