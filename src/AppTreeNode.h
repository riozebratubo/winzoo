#pragma once
#include <windows.h>
#include <string>
#include <vector>

// A node in the app-menu tree: either a folder (isFolder=true, has children)
// or a leaf app (isFolder=false, has exePath/iconPath/icon).
struct AppTreeNode {
    std::wstring             name;
    bool                     isFolder = false;
    HICON                    icon     = nullptr; // null for folders
    std::wstring             exePath;
    std::wstring             iconPath;
    std::vector<AppTreeNode> children; // non-empty only for folders
};
