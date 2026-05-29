#pragma once
#include <windows.h>
#include <unordered_map>
#include <cstdint>

class IconCache {
public:
    // Returns a shared HICON for the window (do not call DestroyIcon on it).
    // sizePx is the desired logical pixel size (e.g. 16 or 32).
    HICON GetIcon(HWND hwnd, int sizePx);

    void Evict(HWND hwnd);
    void Clear();

    ~IconCache() { Clear(); }

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

    std::unordered_map<Key, HICON, KeyHash> cache_;

    static HICON LoadForWindow(HWND hwnd, int sizePx);
};
