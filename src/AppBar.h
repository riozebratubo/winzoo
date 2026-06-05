#pragma once
#include <windows.h>
#include <shellapi.h>
#include "Settings.h"

class AppBar {
public:
    bool Register(HWND hwnd, TaskbarPosition position, int thicknessPx);
    bool SetPosition(TaskbarPosition position, int thicknessPx);
    void Unregister();

    RECT GetReservedRect() const { return reservedRect_; }
    bool IsRegistered()    const { return registered_; }
    UINT CallbackMessage() const { return callbackMsg_; }

    void OnCallback(WPARAM wParam, LPARAM lParam);

    ~AppBar() { Unregister(); }

private:
    HWND            hwnd_         = nullptr;
    APPBARDATA      abd_          = {};
    RECT            reservedRect_ = {};
    bool            registered_   = false;
    TaskbarPosition position_     = TaskbarPosition::Bottom;
    int             thicknessPx_  = 48;
    UINT            callbackMsg_  = 0;

    static UINT EdgeForPosition(TaskbarPosition p);
    RECT MonitorRectForWindow() const;

    bool            adjustingWorkArea_ = false;
};
