#include "IconCache.h"
#include <shellapi.h>

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
    for (;;) {
        Request req;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
            if (stop_ && queue_.empty()) return;
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

HICON IconCache::LoadForWindow(HWND hwnd, int sizePx)
{
    HICON icon = nullptr;

    // 1. WM_GETICON (ICON_SMALL2 = best small, ICON_BIG for large)
    UINT iconType = (sizePx <= 20) ? ICON_SMALL2 : ICON_BIG;
    DWORD_PTR result = 0;
    if (SendMessageTimeout(hwnd, WM_GETICON, iconType, 0,
                           SMTO_ABORTIFHUNG, 50, &result) && result)
        icon = CopyIcon(reinterpret_cast<HICON>(result));

    // 1b. Retry with ICON_SMALL if ICON_SMALL2 yielded nothing
    if (!icon && iconType == ICON_SMALL2) {
        result = 0;
        if (SendMessageTimeout(hwnd, WM_GETICON, ICON_SMALL, 0,
                               SMTO_ABORTIFHUNG, 50, &result) && result)
            icon = CopyIcon(reinterpret_cast<HICON>(result));
    }

    // 2. Class small icon
    if (!icon) {
        HICON cls = reinterpret_cast<HICON>(GetClassLongPtr(hwnd, GCLP_HICONSM));
        if (cls) icon = CopyIcon(cls);
    }

    // 3. Class large icon
    if (!icon) {
        HICON cls = reinterpret_cast<HICON>(GetClassLongPtr(hwnd, GCLP_HICON));
        if (cls) icon = CopyIcon(cls);
    }

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
                // Prefer the size closest to the request; fall back to the other
                if (sizePx <= 20)
                    icon = sm ? sm : lg;
                else
                    icon = lg ? lg : sm;
                // Destroy the one we don't use
                if (icon != lg && lg) DestroyIcon(lg);
                if (icon != sm && sm) DestroyIcon(sm);
            }
        }
    }

    // 5. System fallback
    if (!icon)
        icon = CopyIcon(reinterpret_cast<HICON>(LoadImage(nullptr, IDI_APPLICATION,
                                                  IMAGE_ICON, sizePx, sizePx,
                                                  LR_SHARED)));

    return icon;
}
