#include "AppFolderWatcher.h"
#include <shlobj.h>

void AppFolderWatcher::Start(HWND hwnd, UINT msg)
{
    Stop();
    hwnd_ = hwnd;
    msg_  = msg;

    const int roots[2] = { CSIDL_PROGRAMS, CSIDL_COMMON_PROGRAMS };
    for (int i = 0; i < 2; ++i) {
        wchar_t path[MAX_PATH] = {};
        if (FAILED(SHGetFolderPathW(nullptr, roots[i], nullptr,
                                    SHGFP_TYPE_CURRENT, path)))
            continue;
        HANDLE change = FindFirstChangeNotificationW(path, TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
            FILE_NOTIFY_CHANGE_LAST_WRITE);
        if (change == INVALID_HANDLE_VALUE)
            continue;
        watches_[i].change = change;
        watches_[i].self   = this;
        // Repeating threadpool wait; OnChange re-arms the notification each time.
        if (!RegisterWaitForSingleObject(&watches_[i].wait, change, OnChange,
                                         &watches_[i], INFINITE, WT_EXECUTEDEFAULT)) {
            FindCloseChangeNotification(change);
            watches_[i] = {};
        }
    }
}

void AppFolderWatcher::Stop()
{
    for (auto& w : watches_) {
        // Blocking unregister so no callback can touch this object afterwards.
        if (w.wait)   UnregisterWaitEx(w.wait, INVALID_HANDLE_VALUE);
        if (w.change) FindCloseChangeNotification(w.change);
        w = {};
    }
    hwnd_ = nullptr;
    msg_  = 0;
}

void CALLBACK AppFolderWatcher::OnChange(PVOID ctx, BOOLEAN /*timedOut*/)
{
    auto* w = static_cast<Watch*>(ctx);
    // Re-arm first: the handle stays signaled until FindNextChangeNotification,
    // and a signaled handle would spin the repeating wait.
    if (!FindNextChangeNotification(w->change))
        return;  // watch died — the slow periodic rescan still covers us
    if (w->self->hwnd_ && w->self->msg_)
        PostMessageW(w->self->hwnd_, w->self->msg_, 0, 0);
}
