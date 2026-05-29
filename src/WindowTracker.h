#pragma once
#include <windows.h>
#include <vector>
#include <functional>
#include "TaskButton.h"
#include "IconCache.h"

class WindowTracker {
public:
    using ChangeCallback = std::function<void()>;

    bool Initialize(HWND hwndSink, IconCache* cache, ChangeCallback onChange);
    void Shutdown();

    void OnShellMessage(WPARAM wParam, LPARAM lParam);
    void UpdateActiveWindow();
    void RefreshTitle(HWND hwnd);

    const std::vector<TaskButton>& Buttons()        const { return buttons_; }
    std::vector<TaskButton>&       MutableButtons()       { return buttons_; }

    UINT ShellHookMessage() const { return shellHookMsg_; }

    bool ShouldTrack(HWND hwnd) const;

private:
    void Seed();
    void AddWindow(HWND hwnd);
    void RemoveWindow(HWND hwnd);
    int  FindByHwnd(HWND hwnd) const;
    std::wstring GetWindowTitle(HWND hwnd) const;

    std::vector<TaskButton> buttons_;
    HWND                    hwndSink_    = nullptr;
    IconCache*              iconCache_   = nullptr;
    ChangeCallback          onChange_;
    UINT                    shellHookMsg_ = 0;

    static constexpr int kIconSize = 16;
};
