#pragma once
#include <windows.h>
#include <string>

// Intercepts ITaskbarList3 progress in two complementary ways:
//  1. HKCU COM override: winzoo_com.dll is loaded instead of shell32's CTaskbarList
//     for any app that calls CoCreateInstance(CLSID_TaskbarList) after Winzoo starts.
//  2. Proxy Shell_TrayWnd window: shell32's CTaskbarList always FindWindow("Shell_TrayWnd")
//     and sends private messages. A HWND_TOPMOST window with that class name is found
//     first, intercepting progress from ALL apps including Explorer's own copy engine.
// Both paths relay via the WinzooProgress registered message to all WinzooTaskbar windows.
class TaskbarProxy {
public:
    bool Install(HWND winzooHwnd);
    void Uninstall();
    void UpdatePosition(RECT screenRect);

    UINT RelayMsg() const { return relayMsg_; }

    static HWND DecodeHwnd   (WPARAM wp) { return reinterpret_cast<HWND>(wp); }
    static int  DecodeState  (LPARAM lp) { return static_cast<int>(LOWORD(lp)); }
    static int  DecodePercent(LPARAM lp) { return static_cast<int>(HIWORD(lp)); }

    ~TaskbarProxy() { Uninstall(); }

private:
    static LRESULT CALLBACK ProxyWndProc(HWND, UINT, WPARAM, LPARAM);

    HWND         winzooHwnd_      = nullptr;
    HWND         proxyHwnd_      = nullptr;
    HWND         explorerTray_   = nullptr;
    UINT         relayMsg_       = 0;
    bool         registered_     = false;
    bool         updatingPosition_ = false;
    HMODULE      hHookDll_       = nullptr;
    std::wstring dllPath_;
};
