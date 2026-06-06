#pragma once
#include "Settings.h"
#include <windows.h>

// Method B ("Before Explorer"): dock Explorer's own taskbar on the same edge as
// winzoo. ApplicationFrameWindow-hosted apps (the new Task Manager, Settings, and
// other UWP/WinUI apps) resolve the taskbar location from Explorer's *registered*
// appbar — not from the per-window ptMinPosition or the Shell_TrayWnd window rect
// that steer normal apps — so their minimize animations fly toward Explorer's old
// edge. Relocating Explorer's appbar to match winzoo fixes those apps.
//
// Implementation: patch the docked-edge byte in HKCU StuckRects3\Settings and,
// only when the edge actually changed, gracefully restart explorer.exe so it
// re-reads the value. Every failure path is non-fatal (winzoo keeps running, the
// minimize direction simply stays unfixed for AFW apps). Returns true if Explorer
// was relocated/restarted.
bool RelocateExplorerTaskbarToMatch(TaskbarPosition position);

// SW_HIDE on Shell_TrayWnd hides the window but leaves Explorer's taskbar registered
// as an appbar, so the shell keeps reserving its strip of the work area — windows
// behave as if a taskbar is still there, and the leftover appbar keeps reasserting its
// edge (fighting winzoo's bar). These two best-effort helpers remove that reservation
// when winzoo takes over and re-add it when winzoo exits, so the desktop work area is
// correct both while winzoo runs and after it quits. Both no-op on a null/invalid tray.
void RemoveShellAppBarReservation(HWND tray);
void RestoreShellAppBarReservation(HWND tray);

// Hide Explorer's taskbars on ALL monitors (primary Shell_TrayWnd + every
// Shell_SecondaryTrayWnd) and drop their appbar reservations. winzoo's own proxy
// Shell_TrayWnd is left alone. Only acts on currently-visible trays, so it's cheap to
// call repeatedly — needed because Windows 11 re-creates/re-shows secondary taskbars on
// display and work-area changes (including the work-area change winzoo itself makes),
// so a one-shot hide at startup doesn't stick. Call at startup, on TaskbarCreated, and
// periodically.
void HideExplorerTaskbars();
