#pragma once

// Optional StartAllBack-style feature: restore the Windows 10 File Explorer "ribbon" —
// the classic command strip across the top of folder windows — on Windows 11.
//
// Mechanism (no DLL injection): register an HKCU\Software\Classes\CLSID redirect that
// points the new Windows 11 File-Explorer view-adapter CLSIDs at a non-existent DLL, so
// the modern XAML command bar fails to load and Explorer falls back to the legacy ribbon.
// HKCU\Software\Classes overrides the real HKLM registration, so the change is per-user
// and fully reversible by deleting the keys. This is the same HKCU CLSID-redirect trick
// winzoo already uses for its taskbar COM proxy (see TaskbarProxy.cpp).
//
// Takes effect only after an Explorer restart. Trade-offs the UI must surface: File
// Explorer tabs are unavailable while enabled, and the fallback may stop working after a
// future Windows update (it is an unsupported legacy path Microsoft is removing).
//
// The live registry is the source of truth (not a winzoo Settings field), so the UI
// reflects IsEnabled() rather than a saved flag — this avoids drift if the user toggles
// the same keys with another tool.
namespace ExplorerRibbon {

// Are the redirect keys currently present and pointing at our sentinel path?
bool IsEnabled();

// Write (on) or delete (off) the redirect keys. Does NOT restart Explorer.
// Returns true if every key operation succeeded.
bool SetEnabled(bool on);

// SetEnabled(on) followed by a graceful Explorer restart so the change takes effect.
void Apply(bool on);

}  // namespace ExplorerRibbon
