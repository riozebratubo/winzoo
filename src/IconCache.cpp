#include "IconCache.h"
#include <shellapi.h>

HICON IconCache::GetIcon(HWND hwnd, int sizePx)
{
    Key key{ hwnd, sizePx };
    auto it = cache_.find(key);
    if (it != cache_.end())
        return it->second;

    HICON icon = LoadForWindow(hwnd, sizePx);
    cache_[key] = icon;
    return icon;
}

void IconCache::Evict(HWND hwnd)
{
    for (auto it = cache_.begin(); it != cache_.end(); ) {
        if (it->first.hwnd == hwnd) {
            if (it->second) DestroyIcon(it->second);
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }
}

void IconCache::Clear()
{
    for (auto& [key, icon] : cache_)
        if (icon) DestroyIcon(icon);
    cache_.clear();
}

HICON IconCache::LoadForWindow(HWND hwnd, int sizePx)
{
    HICON icon = nullptr;

    // 1. WM_GETICON (ICON_SMALL2 = best small, ICON_BIG for large)
    UINT iconType = (sizePx <= 20) ? ICON_SMALL2 : ICON_BIG;
    DWORD_PTR result = 0;
    if (SendMessageTimeout(hwnd, WM_GETICON, iconType, 0,
                           SMTO_ABORTIFHUNG, 50, &result) && result)
        icon = reinterpret_cast<HICON>(result);

    // 2. Class small icon
    if (!icon)
        icon = reinterpret_cast<HICON>(GetClassLongPtr(hwnd, GCLP_HICONSM));

    // 3. Class large icon
    if (!icon)
        icon = reinterpret_cast<HICON>(GetClassLongPtr(hwnd, GCLP_HICON));

    // 4. Extract from executable
    if (!icon) {
        wchar_t path[MAX_PATH] = {};
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hProc) {
            DWORD len = MAX_PATH;
            QueryFullProcessImageNameW(hProc, 0, path, &len);
            CloseHandle(hProc);
        }
        if (*path) {
            HICON lg = nullptr, sm = nullptr;
            if (ExtractIconExW(path, 0, &lg, &sm, 1) > 0) {
                icon = (sizePx <= 20) ? sm : lg;
                // Destroy the one we don't use
                if (icon == sm && lg) DestroyIcon(lg);
                if (icon == lg && sm) DestroyIcon(sm);
            }
        }
    }

    // 5. System fallback
    if (!icon)
        icon = reinterpret_cast<HICON>(LoadImage(nullptr, IDI_APPLICATION,
                                                  IMAGE_ICON, sizePx, sizePx,
                                                  LR_SHARED));

    return icon;
}
