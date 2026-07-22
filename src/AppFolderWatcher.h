#pragma once
#include <windows.h>

// Watches the two Start Menu Programs folders AppScanner::Scan() walks (the
// per-user and all-users roots) and posts `msg` to `hwnd` whenever anything
// under them changes. Replaces the fixed 30s rescan poll: the taskbar
// debounces the message and rescans only when an install/uninstall actually
// touched the Start Menu. Notifications arrive via threadpool waits, so no
// dedicated thread is needed.
class AppFolderWatcher {
public:
    void Start(HWND hwnd, UINT msg);
    void Stop();
    ~AppFolderWatcher() { Stop(); }

private:
    static void CALLBACK OnChange(PVOID ctx, BOOLEAN timedOut);

    struct Watch {
        HANDLE            change = nullptr;  // FindFirstChangeNotification handle
        HANDLE            wait   = nullptr;  // RegisterWaitForSingleObject handle
        AppFolderWatcher* self   = nullptr;
    };
    Watch watches_[2];
    HWND  hwnd_ = nullptr;
    UINT  msg_  = 0;
};
