#include "AppIconCache.h"
#include <shellapi.h>

void AppIconCache::ParseIconPath(const std::wstring& raw,
                                  std::wstring& outPath, int& outIndex)
{
    outIndex = 0;
    if (raw.empty()) { outPath = raw; return; }

    size_t pos = raw.rfind(L',');
    if (pos == std::wstring::npos) { outPath = raw; return; }

    // Check if everything after the comma is an optional minus + digits
    size_t i = pos + 1;
    if (i < raw.size() && raw[i] == L'-') ++i;
    bool allDigits = (i < raw.size());
    for (size_t j = i; j < raw.size(); ++j) {
        if (raw[j] < L'0' || raw[j] > L'9') { allDigits = false; break; }
    }

    if (!allDigits) { outPath = raw; return; }

    outPath  = raw.substr(0, pos);
    // _wtoi handles negative sign correctly; no extra negation needed
    outIndex = _wtoi(raw.c_str() + pos + 1);
}

HICON AppIconCache::LoadStatic(const std::wstring& iconPath, int sizePx)
{
    if (iconPath.empty()) return nullptr;

    std::wstring path;
    int          index = 0;
    ParseIconPath(iconPath, path, index);

    if (path.empty()) return nullptr;

    // Expand environment strings
    wchar_t expanded[MAX_PATH * 2];
    ExpandEnvironmentStringsW(path.c_str(), expanded, MAX_PATH * 2);

    // Try ExtractIconExW for specific index
    HICON hLarge = nullptr, hSmall = nullptr;
    UINT count = ExtractIconExW(expanded, index, &hLarge, &hSmall, 1);
    if (count > 0) {
        if (sizePx <= 16) {
            if (hLarge) { DestroyIcon(hLarge); hLarge = nullptr; }
            return hSmall;
        } else {
            if (hSmall) { DestroyIcon(hSmall); hSmall = nullptr; }
            return hLarge;
        }
    }

    // Fallback: SHGetFileInfoW (handles .exe, .lnk, etc.)
    SHFILEINFOW sfi = {};
    DWORD_PTR hr = SHGetFileInfoW(expanded, 0, &sfi, sizeof(sfi),
                                   SHGFI_ICON |
                                   (sizePx <= 16 ? SHGFI_SMALLICON : SHGFI_LARGEICON));
    if (hr && sfi.hIcon) return sfi.hIcon;

    return nullptr;
}

HICON AppIconCache::TryGet(const std::wstring& iconPath, int sizePx) const
{
    Key k{ iconPath, sizePx };
    auto it = cache_.find(k);
    return it != cache_.end() ? it->second : nullptr;
}

HICON AppIconCache::Get(const std::wstring& iconPath, int sizePx)
{
    Key k{ iconPath, sizePx };
    auto it = cache_.find(k);
    if (it != cache_.end()) return it->second;

    HICON icon = LoadStatic(iconPath, sizePx);
    cache_.emplace(std::move(k), icon);
    return icon;
}

void AppIconCache::Store(const std::wstring& iconPath, int sizePx, HICON icon)
{
    Key k{ iconPath, sizePx };
    auto it = cache_.find(k);
    if (it == cache_.end())
        cache_.emplace(std::move(k), icon);
    // If already present, discard the incoming icon to avoid a leak.
    else if (icon)
        DestroyIcon(icon);
}

void AppIconCache::LoadAll(std::vector<AppEntry>& entries, int sizePx)
{
    for (auto& e : entries) {
        const std::wstring& path = e.iconPath.empty() ? e.exePath : e.iconPath;
        e.icon = Get(path, sizePx);
    }
}

void AppIconCache::Clear()
{
    for (auto& [k, icon] : cache_) {
        if (icon) DestroyIcon(icon);
    }
    cache_.clear();
}
