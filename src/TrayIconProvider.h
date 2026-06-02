#pragma once
#include <windows.h>
#include <string>
#include <vector>

// One notification-area icon enumerated from Explorer's toolbar.
struct TrayIconEntry {
    HWND         hWnd;         // icon's callback window
    UINT         uID;          // icon ID
    UINT         uCallbackMsg; // message posted to hWnd on interaction
    HICON        hIcon;        // caller must DestroyIcon when done
    std::wstring tooltip;      // tooltip text
    std::wstring exeName;      // basename e.g. "Discord.exe"
    std::wstring orderKey;     // exeName + "|" + str(uID), stable per-session key
};

// Enumerate visible icons from the Windows notification area toolbar.
// iconSizePx is the requested icon size in physical pixels.
// fallbackExeIcon: if true, falls back to extracting the app's exe icon when other methods fail.
// Returns empty vector if the toolbar cannot be located (e.g. Windows 11 XAML tray).
std::vector<TrayIconEntry> EnumerateTrayIcons(int iconSizePx, bool fallbackExeIcon = false);
