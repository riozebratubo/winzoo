#pragma once
#include <windows.h>
#include "Settings.h"

class SettingsDialog {
public:
    // Returns true if user pressed OK and settings were modified.
    static bool Show(HWND hwndParent, Settings& settings);

private:
    static INT_PTR CALLBACK DlgProc(HWND, UINT, WPARAM, LPARAM);
};
