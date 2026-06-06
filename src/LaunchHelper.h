#pragma once
#include <windows.h>

// No-op — kept for API compatibility.
void RegisterLaunchHelperClass(HINSTANCE hInst);

// Launches exe via ShellExecuteExW and, in a background thread, moves the
// app's first visible top-level window to the center of hMon.
void LaunchOnMonitor(HINSTANCE hInst, HMONITOR hMon,
                     const wchar_t* exe, const wchar_t* args, int nShow);

// Launches exe and moves whatever window becomes foreground to hMon.
// Works for singleton apps (e.g. Task Manager) that reuse an existing window.
void LaunchOrActivateOnMonitor(HWND hwndCaller, HMONITOR hMon,
                               const wchar_t* exe, const wchar_t* args, int nShow);
