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

    // (Re)install the Explorer-thread hook (progress + tray interception). Safe to
    // call repeatedly — e.g. from the TaskbarCreated handler after Explorer restarts,
    // which is exactly when the initial install can miss (Explorer mid-restart).
    // Returns true if a fresh hook was installed on this call.
    bool EnsureExplorerHook();

    // Mark that a TaskbarCreated broadcast we initiated is about to arrive, so
    // RecentSelfTaskbarCreated() returns true and winzoo answers its own echo with a
    // cheap Z-order re-assert instead of a full appbar teardown (which flickers). Call
    // this right before triggering the de-elevated re-broadcast helper — that helper
    // runs in a separate (medium-IL) process and so cannot set this tick itself.
    void MarkSelfBroadcastPending() { selfBroadcastTick_ = GetTickCount(); }

    // True while a TaskbarCreated broadcast WE posted may still be in flight
    // (EnsureExplorerHook broadcasts one after installing the hook so apps
    // re-register their tray icons — including on every normal startup). Lets the
    // TaskbarCreated handler tell our own broadcast apart from a real Explorer
    // restart and skip the disruptive appbar teardown for it. Static so every bar
    // can query it, not just the primary that owns the proxy. Cleared the moment
    // the hooked Explorer process exits, so a genuine restart is never mistaken.
    static bool RecentSelfTaskbarCreated();

    UINT RelayMsg() const { return relayMsg_; }

    static HWND DecodeHwnd   (WPARAM wp) { return reinterpret_cast<HWND>(wp); }
    static int  DecodeState  (LPARAM lp) { return static_cast<int>(LOWORD(lp)); }
    static int  DecodePercent(LPARAM lp) { return static_cast<int>(HIWORD(lp)); }

    ~TaskbarProxy() { Uninstall(); }

private:
    static LRESULT CALLBACK ProxyWndProc(HWND, UINT, WPARAM, LPARAM);

    // Threadpool wait on the hooked Explorer's process handle: the moment that
    // Explorer exits, post the WinzooExplorerGone registered message to
    // winzooHwnd_ so the taskbar drives its watchdog pass immediately instead
    // of discovering the restart on a slow poll.
    void WatchExplorerProcess(DWORD pid);
    void CancelExplorerWatch();
    static void CALLBACK ExplorerExitCallback(PVOID ctx, BOOLEAN timedOut);

    HWND         winzooHwnd_      = nullptr;
    HWND         proxyHwnd_      = nullptr;
    HWND         explorerTray_   = nullptr;
    UINT         relayMsg_       = 0;
    UINT         explorerGoneMsg_ = 0;
    bool         registered_     = false;
    bool         updatingPosition_ = false;
    HMODULE      hHookDll_       = nullptr;
    DWORD        hookedExplorerPid_ = 0;   // pid of the Explorer we currently hook (0 = none)
    HANDLE       explorerProc_   = nullptr;
    HANDLE       explorerWait_   = nullptr;
    std::wstring dllPath_;

    static DWORD selfBroadcastTick_;   // GetTickCount of our last TaskbarCreated broadcast (0 = none)
};
