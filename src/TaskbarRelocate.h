#pragma once
#include "Settings.h"

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
