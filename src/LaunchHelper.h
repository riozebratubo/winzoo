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

// ShellExecute that launches at the user's (medium) integrity level when winzoo
// is running elevated, so launched apps don't inherit our admin token. Falls
// back to a direct ShellExecute when not elevated. Returns true on success.
bool ShellExecuteUser(HWND hwnd, const wchar_t* verb, const wchar_t* file,
                      const wchar_t* params, const wchar_t* dir, int nShow);
