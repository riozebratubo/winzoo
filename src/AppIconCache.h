#pragma once
#include <windows.h>
#include <string>
#include <unordered_map>
#include <vector>
#include "AppEntry.h"

class AppIconCache {
public:
    // Returns cached icon without loading; returns nullptr if not yet cached.
    HICON TryGet(const std::wstring& iconPath, int sizePx) const;

    // Returns a cached HICON for the given icon path, or nullptr.
    // Do NOT call DestroyIcon on the returned handle.
    HICON Get(const std::wstring& iconPath, int sizePx);

    // Stores a pre-loaded icon (used to ingest results from a background thread).
    // Ownership of the HICON transfers to the cache; a nullptr is also valid.
    void Store(const std::wstring& iconPath, int sizePx, HICON icon);

    // Loads icons for all entries that don't already have one.
    void LoadAll(std::vector<AppEntry>& entries, int sizePx);

    // Clears all cached icons (call before rebuilding).
    void Clear();

    ~AppIconCache() { Clear(); }

    // Thread-safe icon loader — no shared state, safe to call from any thread.
    static HICON LoadStatic(const std::wstring& iconPath, int sizePx);

private:
    struct Key {
        std::wstring path;
        int          size;
        bool operator==(const Key& o) const { return size == o.size && path == o.path; }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const noexcept {
            std::size_t h1 = std::hash<std::wstring>{}(k.path);
            std::size_t h2 = std::hash<int>{}(k.size);
            return h1 ^ (h2 << 16);
        }
    };

    std::unordered_map<Key, HICON, KeyHash> cache_;

    // Parses "path,index" into path + index components.
    static void ParseIconPath(const std::wstring& raw,
                               std::wstring& outPath, int& outIndex,
                               bool& outExplicitIndex);
};
