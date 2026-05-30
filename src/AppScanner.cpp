#include "AppScanner.h"
#include <shlobj.h>
#include <algorithm>
#include <string>

// Compare two entries by (folderPath, name) for stable sort + dedup.
static int CompareEntries(const AppEntry& a, const AppEntry& b)
{
    size_t minLen = std::min(a.folderPath.size(), b.folderPath.size());
    for (size_t i = 0; i < minLen; ++i) {
        int cmp = _wcsicmp(a.folderPath[i].c_str(), b.folderPath[i].c_str());
        if (cmp != 0) return cmp;
    }
    if (a.folderPath.size() != b.folderPath.size())
        return (a.folderPath.size() < b.folderPath.size()) ? -1 : 1;
    return _wcsicmp(a.name.c_str(), b.name.c_str());
}

// Recursively find all .lnk files under dir, preserving the subfolder path.
static void EnumerateLnkFiles(const std::wstring& dir,
                               const std::vector<std::wstring>& folderPath,
                               std::vector<AppEntry>& out)
{
    WIN32_FIND_DATAW fd = {};
    HANDLE hFind = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
                std::vector<std::wstring> sub = folderPath;
                sub.push_back(fd.cFileName);
                EnumerateLnkFiles(dir + L"\\" + fd.cFileName, sub, out);
            }
        } else {
            size_t len = wcslen(fd.cFileName);
            if (len > 4 && _wcsicmp(fd.cFileName + len - 4, L".lnk") == 0) {
                std::wstring fullPath = dir + L"\\" + fd.cFileName;
                AppEntry e;
                e.name       = std::wstring(fd.cFileName, len - 4);
                e.iconPath   = fullPath;
                e.exePath    = fullPath;
                e.folderPath = folderPath;
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
        EnumerateLnkFiles(userPrograms, {}, entries);

    // All-users Start Menu programs folder
    wchar_t commonPrograms[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_PROGRAMS, nullptr, SHGFP_TYPE_CURRENT, commonPrograms)))
        EnumerateLnkFiles(commonPrograms, {}, entries);

    // Sort by (folderPath, name) so duplicates between user and common are adjacent.
    std::sort(entries.begin(), entries.end(),
              [](const AppEntry& a, const AppEntry& b) {
                  return CompareEntries(a, b) < 0;
              });

    // Remove duplicates — same relative path (folder + name) from both roots.
    entries.erase(
        std::unique(entries.begin(), entries.end(),
                    [](const AppEntry& a, const AppEntry& b) {
                        return CompareEntries(a, b) == 0;
                    }),
        entries.end());

    return entries;
}
