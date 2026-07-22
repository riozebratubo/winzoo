#pragma once
#include <windows.h>
#include <vector>
#include <functional>
#include <unordered_map>
#include "TaskButton.h"
#include "IconCache.h"

class WindowTracker {
public:
    using ChangeCallback = std::function<void()>;

    bool Initialize(HWND hwndSink, IconCache* cache, ChangeCallback onChange);
    void Shutdown();

    void OnShellMessage(WPARAM wParam, LPARAM lParam);
    // Push-model input from the global WinEvent hooks (WinEventNotifier).
    // Handles what it can immediately (add/remove/title/active) and returns
    // true when the event may need a Reconcile() pass (grace-period removals,
    // restore-from-minimize repaints) so the owner schedules a debounced sweep.
    bool OnWinEvent(DWORD event, HWND hwnd);
    void UpdateActiveWindow();

    // Pixel size at which window icons are captured. Set this to the on-screen
    // icon draw size (Scale(appButtonIconSize, dpi)) so DrawIconEx blits close to
    // 1:1 instead of upscaling a tiny 16 px capture into a blurry button. Changing
    // it re-captures every tracked window's icon at the new size in the background.
    void SetIconSize(int px);
    void RefreshTitle(HWND hwnd);
    void RefreshIcon(HWND hwnd);
    // Swap an asynchronously-resolved icon into the matching button (no-op if
    // the window is gone). The icon is owned by IconCache; not copied here.
    void SetIcon(HWND hwnd, HICON icon);

    // Safety-net sweep: shell-hook and WinEvent notifications can be missed or
    // spurious (windows created without a title yet, events dropped under load).
    // Reconcile re-scans top-level windows, adding trackable ones we missed and
    // dropping buttons whose window no longer exists. Runs debounced after
    // pushed events and on a slow heartbeat — no longer on a fast fixed timer.
    // Returns true when some window is inside its removal grace period, i.e.
    // another pass is needed soon to resolve it (there may be no further events
    // for that window).
    bool Reconcile();

    const std::vector<TaskButton>& Buttons()        const { return buttons_; }
    std::vector<TaskButton>&       MutableButtons()       { return buttons_; }

    UINT ShellHookMessage() const { return shellHookMsg_; }

    bool ShouldTrack(HWND hwnd) const;

private:
    void Seed();
    bool AddWindowInternal(HWND hwnd);   // returns true if a new button was added
    void AddWindow(HWND hwnd);
    void RemoveWindow(HWND hwnd);
    int  FindByHwnd(HWND hwnd) const;
    std::wstring GetWindowTitle(HWND hwnd) const;

    std::vector<TaskButton> buttons_;
    HWND                    hwndSink_    = nullptr;
    IconCache*              iconCache_   = nullptr;
    ChangeCallback          onChange_;
    UINT                    shellHookMsg_ = 0;

    // Per-window tick (GetTickCount64) of when the window was first seen alive but
    // not trackable (and not merely minimized). A button is only dropped once it has
    // stayed that way for kStaleGraceMs, so a momentary blip during an animation
    // never removes it. Time-based (not pass-counted) because Reconcile() now runs
    // at an irregular, event-driven cadence.
    std::unordered_map<HWND, ULONGLONG> staleSince_;

    int iconSizePx_ = 16;   // capture size; overwritten via SetIconSize() before Seed()
    static constexpr ULONGLONG kStaleGraceMs = 800;
};
