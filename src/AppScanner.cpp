#include "AppScanner.h"
#include <shlobj.h>
#include <algorithm>
#include <string>

// Recursively find all .lnk files under dir and add them as AppEntry objects.
static void EnumerateLnkFiles(const std::wstring& dir, std::vector<AppEntry>& out)
{
    WIN32_FIND_DATAW fd = {};
    HANDLE hFind = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0)
                EnumerateLnkFiles(dir + L"\\" + fd.cFileName, out);
        } else {
            size_t len = wcslen(fd.cFileName);
            if (len > 4 && _wcsicmp(fd.cFileName + len - 4, L".lnk") == 0) {
                std::wstring fullPath = dir + L"\\" + fd.cFileName;
                AppEntry e;
                // Name: filename without .lnk extension
                e.name = std::wstring(fd.cFileName, len - 4);
                // Both iconPath and exePath point to the .lnk file itself.
                // SHGetFileInfoW resolves the icon from the link target.
                // ShellExecuteW("open", lnkPath) launches the link target.
                e.iconPath = fullPath;
                e.exePath  = fullPath;
                out.push_back(std::move(e));
            }
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

std::vector<AppEntry> AppScanner::Scan()
{
    std::vector<AppEntry> entries;
    entries.reserve(200);

    // User Start Menu programs folder
    wchar_t userPrograms[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROGRAMS, nullptr, SHGFP_TYPE_CURRENT, userPrograms)))
        EnumerateLnkFiles(userPrograms, entries);

    // All-users Start Menu programs folder
    wchar_t commonPrograms[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_PROGRAMS, nullptr, SHGFP_TYPE_CURRENT, commonPrograms)))
        EnumerateLnkFiles(commonPrograms, entries);

    // Sort by name (case-insensitive)
    std::sort(entries.begin(), entries.end(),
              [](const AppEntry& a, const AppEntry& b) {
                  return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
              });

    // Remove exact name duplicates (a lnk in both user and common programs)
    entries.erase(
        std::unique(entries.begin(), entries.end(),
                    [](const AppEntry& a, const AppEntry& b) {
                        return _wcsicmp(a.name.c_str(), b.name.c_str()) == 0;
                    }),
        entries.end());

    return entries;
}
