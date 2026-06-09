#include "IconCache.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

HICON IconCache::GetIcon(HWND hwnd, int sizePx)
{
    Key key{ hwnd, sizePx };
    auto it = cache_.find(key);
    if (it != cache_.end())
        return it->second.icon;   // resolved icon, or nullptr while pending

    // No sink wired up yet — fall back to a synchronous load so the cache still
    // works for any pre-window-creation caller.
    if (!sink_) {
        HICON icon = LoadForWindow(hwnd, sizePx);
        cache_[key] = { icon, 0, false };
        return icon;
    }

    // Insert a pending placeholder and dispatch the probe to a worker. The icon
    // arrives later via OnResolved(); the button shows text-only until then.
    EnsureWorkers();
    uint64_t id = ++nextReqId_;
    cache_[key] = { nullptr, id, true };
    {
        std::lock_guard<std::mutex> lk(mtx_);
        queue_.push_back({ hwnd, sizePx, id });
    }
    cv_.notify_one();
    return nullptr;
}

void IconCache::Refresh(HWND hwnd, int sizePx)
{
    Key key{ hwnd, sizePx };
    auto it = cache_.find(key);
    if (it == cache_.end()) { GetIcon(hwnd, sizePx); return; }  // not cached → normal load
    if (it->second.pending) return;                             // a probe is already coming
    if (!sink_) {                                               // synchronous fallback
        if (it->second.icon) DestroyIcon(it->second.icon);
        it->second.icon = LoadForWindow(hwnd, sizePx);
        return;
    }
    // Keep the current icon displayed; OnResolved swaps in the fresh one and
    // frees the stale icon once the probe completes.
    EnsureWorkers();
    uint64_t id = ++nextReqId_;
    it->second.reqId   = id;
    it->second.pending = true;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        queue_.push_back({ hwnd, sizePx, id });
    }
    cv_.notify_one();
}

void IconCache::EnsureWorkers()
{
    if (started_) return;
    started_ = true;
    // Icon probes are dominated by SendMessageTimeout round-trips and file I/O,
    // not CPU; a few workers let a slow/hung window stall without blocking the
    // others. Mirrors the 4-way fan-out used for app-icon loading.
    constexpr int kWorkers = 3;
    for (int i = 0; i < kWorkers; ++i)
        workers_.emplace_back([this] { WorkerLoop(); });
}

void IconCache::WorkerLoop()
{
    // IShellItemImageFactory (used by LoadForWindow for high-res exe icons) needs
    // COM on this thread. MTA matches the app-icon loader and avoids STA pumping.
    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        Request req;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
            if (stop_ && queue_.empty()) break;
            req = queue_.front();
            queue_.pop_front();
        }
        HICON real = LoadForWindow(req.hwnd, req.size);
        auto* res = new Resolved{ req.hwnd, req.size, req.reqId, real };
        if (!sink_ || !PostMessageW(sink_, msg_, 0, reinterpret_cast<LPARAM>(res))) {
            if (real) DestroyIcon(real);
            delete res;   // sink gone — drop the result
        }
    }
    if (SUCCEEDED(hrCom)) CoUninitialize();
}

bool IconCache::OnResolved(LPARAM lParam, HWND* outHwnd, HICON* outIcon)
{
    std::unique_ptr<Resolved> res(reinterpret_cast<Resolved*>(lParam));
    Key key{ res->hwnd, res->size };
    auto it = cache_.find(key);
    // Discard if the window was evicted or a newer request superseded this one.
    if (it == cache_.end() || !it->second.pending || it->second.reqId != res->reqId) {
        if (res->icon) DestroyIcon(res->icon);
        return false;
    }
    if (it->second.icon) DestroyIcon(it->second.icon);  // free the stale icon (Refresh path)
    it->second.icon    = res->icon;
    it->second.pending = false;
    if (outHwnd) *outHwnd = res->hwnd;
    if (outIcon) *outIcon = res->icon;
    return true;
}

void IconCache::DiscardResolved(LPARAM lParam)
{
    std::unique_ptr<Resolved> res(reinterpret_cast<Resolved*>(lParam));
    if (res && res->icon) DestroyIcon(res->icon);
}

void IconCache::Evict(HWND hwnd)
{
    for (auto it = cache_.begin(); it != cache_.end(); ) {
        if (it->first.hwnd == hwnd) {
            if (it->second.icon) DestroyIcon(it->second.icon);
            it = cache_.erase(it);   // pending entry dropped too; its result is discarded on arrival
        } else {
            ++it;
        }
    }
}

void IconCache::Clear()
{
    for (auto& [key, entry] : cache_)
        if (entry.icon) DestroyIcon(entry.icon);
    cache_.clear();
}

void IconCache::Stop()
{
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_)
        if (t.joinable()) t.join();
    workers_.clear();
    queue_.clear();
    started_ = false;
    stop_    = false;   // allow restart if ever reused
}

// Native pixel width of an icon's bitmap (0 if it can't be measured). Used to
// decide whether a window's own icon is already big enough to draw without the
// blur that comes from upscaling a small icon to the button size.
static int IconPixelWidth(HICON h)
{
    if (!h) return 0;
    ICONINFO ii = {};
    if (!GetIconInfo(h, &ii)) return 0;
    BITMAP bm = {};
    HBITMAP src = ii.hbmColor ? ii.hbmColor : ii.hbmMask;
    int w = (src && GetObjectW(src, sizeof(bm), &bm)) ? bm.bmWidth : 0;
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask)  DeleteObject(ii.hbmMask);
    return w;
}

// Converts a 32-bit ARGB HBITMAP (from IShellItemImageFactory) to an HICON.
static HICON BitmapToIcon(HBITMAP hbm, int size)
{
    HBITMAP hbmMask = CreateBitmap(size, size, 1, 1, nullptr);
    if (!hbmMask) return nullptr;
    ICONINFO ii = { TRUE, 0, 0, hbmMask, hbm };
    HICON hIcon = CreateIconIndirect(&ii);
    DeleteObject(hbmMask);
    return hIcon;
}

// Renders the executable's icon at exactly sizePx via the same shell API the
// taskbar uses for pinned apps — pulls from the 256 px asset (or anti-aliased
// 32 px for legacy apps) so the result is crisp at any button size.
static HICON LoadHiResExeIcon(const wchar_t* path, int sizePx)
{
    HICON icon = nullptr;
    IShellItem* pItem = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(path, nullptr, IID_PPV_ARGS(&pItem)))) {
        IShellItemImageFactory* pSIIF = nullptr;
        if (SUCCEEDED(pItem->QueryInterface(IID_PPV_ARGS(&pSIIF)))) {
            SIZE sz = { sizePx, sizePx };
            HBITMAP hbm = nullptr;
            if (SUCCEEDED(pSIIF->GetImage(sz, (SIIGBF)(SIIGBF_ICONONLY | SIIGBF_SCALEUP), &hbm)) && hbm) {
                icon = BitmapToIcon(hbm, sizePx);
                DeleteObject(hbm);
            }
            pSIIF->Release();
        }
        pItem->Release();
    }
    return icon;
}

// Basename equals ApplicationFrameHost.exe — the host process for packaged/UWP
// apps. Its own exe icon is generic, so for these windows we must keep the
// window's own (accurate) icon rather than substituting the host's exe icon.
static bool IsPackagedAppHost(const wchar_t* path)
{
    const wchar_t* base = path;
    for (const wchar_t* p = path; *p; ++p)
        if (*p == L'\\' || *p == L'/') base = p + 1;
    return _wcsicmp(base, L"ApplicationFrameHost.exe") == 0;
}

HICON IconCache::LoadForWindow(HWND hwnd, int sizePx)
{
    // 1. Gather the window's own icon, keeping the largest available variant.
    //    This is the accurate, app/document-specific icon (and the only correct
    //    source for packaged/UWP apps).
    HICON winIcon = nullptr;
    int   winSize = 0;
    auto consider = [&](HICON src) {
        if (!src) return;
        HICON copy = CopyIcon(src);
        if (!copy) return;
        int w = IconPixelWidth(copy);
        if (w > winSize) {
            if (winIcon) DestroyIcon(winIcon);
            winIcon = copy;
            winSize = w;
        } else {
            DestroyIcon(copy);
        }
    };

    DWORD_PTR result = 0;
    if (SendMessageTimeout(hwnd, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 50, &result) && result)
        consider(reinterpret_cast<HICON>(result));
    consider(reinterpret_cast<HICON>(GetClassLongPtr(hwnd, GCLP_HICON)));
    result = 0;
    if (SendMessageTimeout(hwnd, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG, 50, &result) && result)
        consider(reinterpret_cast<HICON>(result));
    result = 0;
    if (SendMessageTimeout(hwnd, WM_GETICON, ICON_SMALL, 0, SMTO_ABORTIFHUNG, 50, &result) && result)
        consider(reinterpret_cast<HICON>(result));
    consider(reinterpret_cast<HICON>(GetClassLongPtr(hwnd, GCLP_HICONSM)));

    // The window's own icon is already big enough — use it as-is (crisp + accurate).
    if (winIcon && winSize >= sizePx)
        return winIcon;

    // 2. Resolve the owning executable.
    wchar_t path[MAX_PATH] = {};
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        DWORD len = MAX_PATH;
        QueryFullProcessImageNameW(hProc, 0, path, &len);
        CloseHandle(hProc);
    }

    // 3. For ordinary apps, a high-res icon rendered from the exe is the same
    //    artwork at full resolution — prefer it over a small upscaled window icon.
    //    Skipped for the packaged-app host, whose exe icon would be generic.
    if (*path && !IsPackagedAppHost(path)) {
        if (HICON hi = LoadHiResExeIcon(path, sizePx)) {
            if (winIcon) DestroyIcon(winIcon);
            return hi;
        }
    }

    // 4. Otherwise keep the window's own icon (accurate; upscaled at draw time).
    if (winIcon)
        return winIcon;

    // 5. No window icon (e.g. inaccessible process): fall back to ExtractIconEx.
    if (*path) {
        HICON lg = nullptr, sm = nullptr;
        if (ExtractIconExW(path, 0, &lg, &sm, 1) > 0) {
            HICON icon = (sizePx <= 20) ? (sm ? sm : lg) : (lg ? lg : sm);
            if (icon != lg && lg) DestroyIcon(lg);
            if (icon != sm && sm) DestroyIcon(sm);
            if (icon) return icon;
        }
    }

    // 6. System default.
    return CopyIcon(reinterpret_cast<HICON>(LoadImage(nullptr, IDI_APPLICATION,
                                            IMAGE_ICON, sizePx, sizePx, LR_SHARED)));
}
