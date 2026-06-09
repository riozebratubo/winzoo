#pragma once
#include <windows.h>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// System-tool search for the app menu.
//
// The Windows Start menu surfaces classic tools (Device Manager, Disk Management,
// Control Panel applets, …) that are neither Start-menu .lnk shortcuts nor
// ms-settings: pages. This module reproduces that, with two interchangeable
// sources selected by Settings::appMenuSearchSystemDynamic:
//
//   dynamic  (default) — enumerate the "All Tasks" / God-Mode shell folder
//                        (shell:::{ED7BA470-…}); comprehensive and localized.
//   curated            — filter the static kSystemTools[] table below; a small,
//                        dependency-free fallback used when dynamic enumeration
//                        is unavailable or disabled.
// ─────────────────────────────────────────────────────────────────────────────

// One curated fallback entry: friendly name, a ShellExecute-able command, and
// space-separated lowercase search keywords.
struct SystemToolDef {
    const wchar_t* displayName;
    const wchar_t* command;    // e.g. L"devmgmt.msc", L"appwiz.cpl"
    const wchar_t* keywords;
};

extern const SystemToolDef kSystemTools[];
extern const int           kSystemToolsCount;

// A single search hit returned to the app menu.
struct SystemToolHit {
    std::wstring name;       // display name
    std::wstring command;    // launch command (curated hits); empty for shell items
    bool         shellItem;  // true => launch by display name via LaunchSystemToolByName
};

// Kicks off the (one-shot) God-Mode enumeration on a background thread so the
// first dynamic search never blocks the UI thread. Idempotent and cheap to call
// repeatedly; a no-op once enumeration has succeeded. Safe to call when dynamic
// search is disabled (harmless). Call it when the app menu opens.
void PrewarmSystemTools();

// Returns system-tool hits matching lowerQuery (already lowercased), ranked
// best-match first (exact > prefix > substring > keyword > fuzzy) and truncated
// to maxResults. When `dynamic` is true the cached God-Mode names are filtered;
// while that cache is still warming (or if enumeration failed) it transparently
// falls back to the curated kSystemTools[] table. When `dynamic` is false the
// curated table is always used.
std::vector<SystemToolHit> SearchSystemTools(const std::wstring& lowerQuery,
                                             bool dynamic, bool fuzzy,
                                             int maxResults = 6);

// Launches a dynamic ("God Mode") item by its display name, de-elevated via
// Explorer's shell automation when possible. Returns true on success.
bool LaunchSystemToolByName(const std::wstring& name);
