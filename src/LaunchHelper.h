#pragma once
#include <windows.h>

struct IShellDispatch2;

// No-op — kept for API compatibility.
void RegisterLaunchHelperClass(HINSTANCE hInst);

// Returns Explorer's medium-integrity IShellDispatch2 automation object, so any
// launch performed through it (ShellExecute, FolderItem::InvokeVerb) de-elevates
// when winzoo is running as administrator. Caller must Release() the result.
// Returns nullptr if the Explorer automation object is unavailable.
IShellDispatch2* GetExplorerShellDispatch();

// Launches exe (de-elevating via Explorer when winzoo is admin) and, in a
// background thread, finds the app's first visible top-level window, brings it to
// the foreground, and — when hMon is non-null — moves it to the center of hMon.
// Pass a null hMon to activate without relocating.
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

// Shows the Windows "Run" dialog via Explorer's IShellDispatch::FileRun. Routing
// through Explorer (medium integrity) means the dialog — and whatever it launches
// — runs at the user's integrity level instead of inheriting winzoo's admin
// token, and yields the genuine "Run" dialog rather than the rundll32-hosted
// variant (which titles itself "Run DLL"). Falls back to rundll32 shell32.dll,#61
// when the Explorer automation object is unavailable.
void ShowRunDialog(HWND hwnd);
