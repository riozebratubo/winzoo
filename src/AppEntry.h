#pragma once
#include <windows.h>
#include <string>
#include <vector>

struct AppEntry {
    std::wstring name;      // display name (filename without .lnk)
    std::wstring iconPath;  // .lnk path used for icon extraction
    std::wstring exePath;   // .lnk path used for launch
    HICON        icon = nullptr; // populated by AppIconCache::LoadAll
    // Path segments from the Start Menu Programs root to the parent folder,
    // e.g. {"Microsoft Office"} for Programs\Microsoft Office\Word.lnk
    std::vector<std::wstring> folderPath;
};
