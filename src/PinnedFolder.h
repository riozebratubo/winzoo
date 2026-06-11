#pragma once
#include <string>
#include <vector>

// Pinned folders are stored in-band in the existing pin lists
// (Settings::pinnedExePaths / pinnedExePathsPerMonitor). Each list element is
// EITHER a plain app path (unchanged, backward compatible) OR a folder encoded
// in one string using control char 0x01 — which can never appear in a file
// path — as both the marker and the field delimiter:
//
//     \x01 'F' <folderName> \x01 <child1> \x01 <child2> ...
//
// An empty folder is just "\x01F<name>". The folder may optionally override its
// icon with one of its children's icons ("cover"); the cover path is appended to
// the name field, separated by control char 0x02:
//
//     \x01 'F' <folderName> \x02 <coverPath> \x01 <child1> ...
//
// Because the whole folder is a single string, persistence (REG_MULTI_SZ in
// Registry.cpp, JSON in SettingsFile.cpp) and the existing drag-reorder logic
// treat it as an opaque token and need no changes.

namespace pinfolder {

inline constexpr wchar_t kSep    = L'\x01';
inline constexpr wchar_t kMarker = L'F';
inline constexpr wchar_t kCover  = L'\x02';  // separates the cover path from the name

// True if `s` encodes a folder (rather than a plain app path).
inline bool IsFolderToken(const std::wstring& s)
{
    return s.size() >= 2 && s[0] == kSep && s[1] == kMarker;
}

// Strip control characters (< 0x20) from a user-entered folder name so they can
// never collide with the kSep delimiter.
inline std::wstring SanitizeName(const std::wstring& name)
{
    std::wstring out;
    out.reserve(name.size());
    for (wchar_t c : name)
        if (c >= 0x20) out.push_back(c);
    return out;
}

// Build a folder token from a (sanitized) name, ordered child paths, and an
// optional cover-icon path.
inline std::wstring MakeFolderToken(const std::wstring& name,
                                    const std::vector<std::wstring>& children,
                                    const std::wstring& cover = L"")
{
    std::wstring token;
    token.push_back(kSep);
    token.push_back(kMarker);
    token += SanitizeName(name);
    if (!cover.empty()) {
        token.push_back(kCover);
        token += cover;
    }
    for (const auto& c : children) {
        token.push_back(kSep);
        token += c;
    }
    return token;
}

// Parse a folder token into name + children + cover. Returns false if `token` is
// not a folder token (outputs left untouched on failure).
inline bool ParseFolderToken(const std::wstring& token,
                             std::wstring& name,
                             std::vector<std::wstring>& children,
                             std::wstring& cover)
{
    if (!IsFolderToken(token)) return false;

    name.clear();
    children.clear();
    cover.clear();

    // Walk fields separated by kSep, starting after the 2-char "\x01F" marker.
    size_t i = 2;
    bool   first = true;  // first field is the name (+ optional cover), rest are children
    while (i <= token.size()) {
        size_t next = token.find(kSep, i);
        std::wstring field = (next == std::wstring::npos)
                                 ? token.substr(i)
                                 : token.substr(i, next - i);
        if (first) {
            size_t cpos = field.find(kCover);
            if (cpos == std::wstring::npos) {
                name = field;
            } else {
                name  = field.substr(0, cpos);
                cover = field.substr(cpos + 1);
            }
            first = false;
        } else if (!field.empty()) {
            children.push_back(field);
        }

        if (next == std::wstring::npos) break;
        i = next + 1;
    }
    return true;
}

// Convenience overload for callers that don't need the cover.
inline bool ParseFolderToken(const std::wstring& token,
                             std::wstring& name,
                             std::vector<std::wstring>& children)
{
    std::wstring dummy;
    return ParseFolderToken(token, name, children, dummy);
}

} // namespace pinfolder
