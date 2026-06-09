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

    // Periodic safety net: shell-hook messages are unreliable (windows can be
    // created without a title yet, or destroy notifications can be missed/spurious).
    // Reconcile re-scans top-level windows, adding trackable ones we missed and
    // dropping buttons whose window no longer exists.
    void Reconcile();

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

    // Per-window count of consecutive Reconcile() ticks the window has been alive but
    // not trackable (and not merely minimized). A button is only dropped once this
    // crosses kStaleThreshold, so a momentary blip during an animation never removes it.
    std::unordered_map<HWND, int> staleTicks_;

    int iconSizePx_ = 16;   // capture size; overwritten via SetIconSize() before Seed()
    static constexpr int kStaleThreshold = 3;   // ~0.75s at the 250ms reconcile cadence
};
