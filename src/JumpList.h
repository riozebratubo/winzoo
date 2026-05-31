#pragma once
#include <windows.h>
#include <string>
#include <vector>

struct JumpListItem {
    std::wstring displayName;
    std::wstring path;
    std::wstring arguments;
    std::wstring workingDir;
    std::wstring category; // e.g. "Tasks", "Pinned" — used as menu section header
    int          showCmd = SW_SHOWNORMAL;
    bool         isFolder = false; // true for folder items (use ShellExecute "explore")
};

// Queries the shell's jump list data for the app identified by the given HWND/exePath.
// Returns at most maxItems results. Returns empty vector on failure.
std::vector<JumpListItem> GetJumpListItems(HWND hwnd, const std::wstring& exePath, int maxItems = 10);
