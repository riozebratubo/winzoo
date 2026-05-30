#pragma once
#include <windows.h>
#include <string>

struct AppEntry {
    std::wstring name;      // DisplayName from registry
    std::wstring iconPath;  // from DisplayIcon value (may include ,index suffix)
    std::wstring exePath;   // executable for launch (stripped of ,index)
    HICON        icon = nullptr; // populated by AppIconCache::LoadAll
};
