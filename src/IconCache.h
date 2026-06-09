#pragma once
#include <windows.h>
#include <unordered_map>
#include <cstdint>
#include <deque>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>

// Resolves window icons off the UI thread. GetIcon() returns immediately:
// a real icon if cached, otherwise nullptr (a "pending" placeholder) while a
// worker thread probes the window (WM_GETICON / class icon / ExtractIconEx) in
// the background. When the probe finishes the worker PostMessage()s the sink
// window; the UI thread then calls OnResolved() to swap the real icon in.
//
// All cache_ access is UI-thread only. The workers touch only the request queue
// (mutex-guarded) and the sink HWND/msg (write-once before any worker starts).
class IconCache {
public:
    // Where resolved icons are reported. Must be called once, before GetIcon(),
    // with a window that handles `msg` by calling OnResolved(lParam, ...).
    void SetNotifySink(HWND sink, UINT msg) { sink_ = sink; msg_ = msg; }

    // Returns the cached icon, or nullptr while a background probe is in flight.
    // Do not call DestroyIcon on the result. sizePx is the desired pixel size.
    HICON GetIcon(HWND hwnd, int sizePx);

    // Re-probe a window's icon in the background, keeping the current icon shown
    // until the new one resolves (no blink). Coalesces if a probe is already in
    // flight. Used for HSHELL_REDRAW title/icon changes.
    void Refresh(HWND hwnd, int sizePx);

    // UI-thread handler for the sink message. lParam is the value posted by a
    // worker. On a still-relevant result, installs the icon and reports the
    // window/icon to update via *outHwnd/*outIcon and returns true. Otherwise
    // (window evicted or a newer request superseded it) frees the icon and
    // returns false.
    bool OnResolved(LPARAM lParam, HWND* outHwnd, HICON* outIcon);

    // Free an in-flight resolved result without installing it (shutdown drain).
    static void DiscardResolved(LPARAM lParam);

    void Evict(HWND hwnd);
    void Clear();

    // Stop and join worker threads. Idempotent; call before destruction.
    void Stop();

    ~IconCache() { Stop(); Clear(); }

private:
    struct Key {
        HWND hwnd;
        int  size;
        bool operator==(const Key& o) const { return hwnd == o.hwnd && size == o.size; }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const noexcept {
            auto h1 = std::hash<HWND>{}(k.hwnd);
            auto h2 = std::hash<int>{}(k.size);
            return h1 ^ (h2 << 16);
        }
    };
    struct Entry {
        HICON    icon;     // resolved icon, or nullptr while pending
        uint64_t reqId;    // identifies the in-flight request (0 = none)
        bool     pending;
    };
    struct Request { HWND hwnd; int size; uint64_t reqId; };
    struct Resolved { HWND hwnd; int size; uint64_t reqId; HICON icon; };

    void EnsureWorkers();
    void WorkerLoop();

    static HICON LoadForWindow(HWND hwnd, int sizePx);

    std::unordered_map<Key, Entry, KeyHash> cache_;   // UI-thread only

    HWND     sink_ = nullptr;   // write-once before workers start
    UINT     msg_  = 0;
    uint64_t nextReqId_ = 0;    // UI-thread only

    std::vector<std::thread>  workers_;
    std::deque<Request>       queue_;   // guarded by mtx_
    std::mutex                mtx_;
    std::condition_variable   cv_;
    bool                      stop_    = false;
    bool                      started_ = false;
};
