#include "JumpList.h"
#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shobjidl_core.h>
#include <propkey.h>
#include <propsys.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <objidl.h>
#include <algorithm>
#include <cctype>

using Microsoft::WRL::ComPtr;

// CLSID for ApplicationDocumentLists
static const CLSID CLSID_AppDocLists =
    { 0x86c14003, 0x4d6b, 0x4ef3, { 0xa7, 0xb4, 0x05, 0x06, 0x66, 0x3b, 0x2e, 0x68 } };

// --------------------------------------------------------------------------
// AUMID discovery strategies
// --------------------------------------------------------------------------

// Strategy 1: Read AUMID from the window's property store
static std::wstring GetAumidFromWindow(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
        return {};

    ComPtr<IPropertyStore> store;
    HRESULT hr = SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store));
    if (FAILED(hr) || !store)
        return {};

    PROPVARIANT pv;
    PropVariantInit(&pv);
    hr = store->GetValue(PKEY_AppUserModel_ID, &pv);
    if (FAILED(hr) || pv.vt != VT_LPWSTR || !pv.pwszVal) {
        PropVariantClear(&pv);
        return {};
    }

    std::wstring appId(pv.pwszVal);
    PropVariantClear(&pv);
    return appId;
}

// Strategy 2: Read AUMID from a .lnk shortcut's property store
static std::wstring GetAumidFromShortcut(const std::wstring& lnkPath)
{
    ComPtr<IShellLinkW> link;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (FAILED(hr)) return {};

    ComPtr<IPersistFile> pf;
    hr = link.As(&pf);
    if (FAILED(hr)) return {};

    hr = pf->Load(lnkPath.c_str(), STGM_READ);
    if (FAILED(hr)) return {};

    ComPtr<IPropertyStore> store;
    hr = link.As(&store);
    if (FAILED(hr) || !store) return {};

    PROPVARIANT pv;
    PropVariantInit(&pv);
    hr = store->GetValue(PKEY_AppUserModel_ID, &pv);
    if (FAILED(hr) || pv.vt != VT_LPWSTR || !pv.pwszVal) {
        PropVariantClear(&pv);
        return {};
    }

    std::wstring appId(pv.pwszVal);
    PropVariantClear(&pv);
    return appId;
}

// Strategy 3: Search Start Menu shortcuts for one targeting the exe
static std::wstring FindShortcutForExe(const std::wstring& exePath)
{
    wchar_t appData[MAX_PATH] = {};
    wchar_t progData[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, CSIDL_PROGRAMS, nullptr, 0, appData);
    SHGetFolderPathW(nullptr, CSIDL_COMMON_PROGRAMS, nullptr, 0, progData);

    std::wstring dirs[] = { appData, progData };
    int shortcutsChecked = 0;
    constexpr int kMaxShortcuts = 150; // Limit to avoid UI lag on right-click

    for (const auto& dir : dirs) {
        if (dir.empty()) continue;

        std::vector<std::wstring> dirsToScan = { dir };

        while (!dirsToScan.empty() && shortcutsChecked < kMaxShortcuts) {
            std::wstring current = dirsToScan.back();
            dirsToScan.pop_back();

            WIN32_FIND_DATAW fd;
            std::wstring pattern = current + L"\\*";
            HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
            if (hFind == INVALID_HANDLE_VALUE) continue;

            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0)
                        dirsToScan.push_back(current + L"\\" + fd.cFileName);
                    continue;
                }

                const wchar_t* ext = PathFindExtensionW(fd.cFileName);
                if (_wcsicmp(ext, L".lnk") != 0) continue;

                if (++shortcutsChecked > kMaxShortcuts) break;

                std::wstring lnkPath = current + L"\\" + fd.cFileName;

                ComPtr<IShellLinkW> link;
                if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr,
                                           CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
                    continue;
                ComPtr<IPersistFile> pf;
                if (FAILED(link.As(&pf))) continue;
                if (FAILED(pf->Load(lnkPath.c_str(), STGM_READ))) continue;

                wchar_t target[MAX_PATH] = {};
                if (SUCCEEDED(link->GetPath(target, MAX_PATH, nullptr, 0)) && target[0]) {
                    if (_wcsicmp(target, exePath.c_str()) == 0) {
                        FindClose(hFind);
                        return lnkPath;
                    }
                }
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }
    }
    return {};
}

// --------------------------------------------------------------------------
// AutomaticDestinations-ms file reader (OLE Structured Storage)
// --------------------------------------------------------------------------
static void ExtractFromShellLink(IShellLinkW* link, JumpListItem& item)
{
    wchar_t buf[MAX_PATH] = {};

    if (SUCCEEDED(link->GetPath(buf, MAX_PATH, nullptr, 0)) && buf[0])
        item.path = buf;

    buf[0] = L'\0';
    if (SUCCEEDED(link->GetArguments(buf, MAX_PATH)) && buf[0])
        item.arguments = buf;

    buf[0] = L'\0';
    if (SUCCEEDED(link->GetWorkingDirectory(buf, MAX_PATH)) && buf[0])
        item.workingDir = buf;

    int showCmd = 0;
    if (SUCCEEDED(link->GetShowCmd(&showCmd)))
        item.showCmd = showCmd;

    // Display name: prefer PKEY_Title (concise jump list label), fallback to Description
    {
        ComPtr<IPropertyStore> ps;
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&ps))) && ps) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(ps->GetValue(PKEY_Title, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
                item.displayName = pv.pwszVal;
            PropVariantClear(&pv);
        }
    }

    if (item.displayName.empty()) {
        buf[0] = L'\0';
        link->GetDescription(buf, MAX_PATH);
        if (buf[0])
            item.displayName = buf;
    }

    // Fall back to file name from path
    if (item.displayName.empty() && !item.path.empty()) {
        const wchar_t* fname = PathFindFileNameW(item.path.c_str());
        if (fname)
            item.displayName = fname;
    }
}

static std::vector<JumpListItem> ReadAutoDestinations(const std::wstring& filePath, int maxItems)
{
    std::vector<JumpListItem> results;

    ComPtr<IStorage> stg;
    HRESULT hr = StgOpenStorage(filePath.c_str(), nullptr,
                                STGM_READ | STGM_SHARE_DENY_WRITE,
                                nullptr, 0, &stg);
    if (FAILED(hr) || !stg)
        return results;

    // Enumerate streams inside the storage.
    // Classic format (Win7/8/10): numbered streams ("0", "1", "2"...) each contain a shell link.
    // New format (Win11+): only DestList + DestListPropertyStore, no per-item streams.
    ComPtr<IEnumSTATSTG> enumStat;
    hr = stg->EnumElements(0, nullptr, 0, &enumStat);
    if (FAILED(hr) || !enumStat)
        return results;

    STATSTG stat;
    while (enumStat->Next(1, &stat, nullptr) == S_OK) {
        if (stat.type != STGTY_STREAM || !stat.pwcsName) {
            if (stat.pwcsName) CoTaskMemFree(stat.pwcsName);
            continue;
        }

        // Skip known metadata streams
        if (_wcsicmp(stat.pwcsName, L"DestList") == 0 ||
            _wcsicmp(stat.pwcsName, L"DestListPropertyStore") == 0) {
            CoTaskMemFree(stat.pwcsName);
            continue;
        }

        ComPtr<IStream> stream;
        hr = stg->OpenStream(stat.pwcsName, nullptr, STGM_READ | STGM_SHARE_EXCLUSIVE, 0, &stream);
        CoTaskMemFree(stat.pwcsName);

        if (FAILED(hr) || !stream) continue;

        // Load a shell link from the stream
        ComPtr<IShellLinkW> link;
        hr = CoCreateInstance(CLSID_ShellLink, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
        if (FAILED(hr)) continue;

        ComPtr<IPersistStream> ps;
        hr = link.As(&ps);
        if (FAILED(hr)) continue;

        hr = ps->Load(stream.Get());
        if (FAILED(hr)) continue;

        JumpListItem item;
        ExtractFromShellLink(link.Get(), item);

        if (item.displayName.empty() && item.path.empty())
            continue;
        if (item.displayName.empty())
            item.displayName = item.path;

        results.push_back(std::move(item));
        if (static_cast<int>(results.size()) >= maxItems)
            break;
    }

    return results;
}

// --------------------------------------------------------------------------
// CustomDestinations-ms file reader (binary format with serialized shell links)
// --------------------------------------------------------------------------
static std::vector<JumpListItem> ReadCustomDestinations(const std::wstring& filePath, int maxItems)
{
    std::vector<JumpListItem> results;

    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return results;

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart < 24 || fileSize.QuadPart > 10 * 1024 * 1024) {
        CloseHandle(hFile);
        return results;
    }

    DWORD size = static_cast<DWORD>(fileSize.QuadPart);
    std::vector<BYTE> data(size);
    DWORD read = 0;
    if (!ReadFile(hFile, data.data(), size, &read, nullptr) || read != size) {
        CloseHandle(hFile);
        return results;
    }
    CloseHandle(hFile);

    // CustomDestinations format:
    // Repeated entries of shell links separated by category headers.
    // Each shell link can be found by scanning for the CLSID_ShellLink binary pattern
    // (the IPersistStream header starts with the CLSID).

    // Shell link serialization magic: the first 4 bytes of a serialized link are 0x0000004C
    // (the size of the SHELL_LINK_HEADER structure, which is always 0x4C = 76 bytes)
    // Followed by the CLSID {00021401-0000-0000-C000-000000000046}
    static const BYTE kLinkClsid[] = {
        0x01, 0x14, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46
    };

    DWORD pos = 0;
    while (pos + 20 < size && static_cast<int>(results.size()) < maxItems) {
        // Scan for link header magic + CLSID
        if (data[pos] != 0x4C || data[pos + 1] != 0x00 || data[pos + 2] != 0x00 || data[pos + 3] != 0x00) {
            ++pos;
            continue;
        }
        if (pos + 20 > size || memcmp(&data[pos + 4], kLinkClsid, 16) != 0) {
            ++pos;
            continue;
        }

        // Found a shell link at this offset — wrap remaining data in a stream
        DWORD remaining = size - pos;
        HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, remaining);
        if (!hGlobal) { ++pos; continue; }

        void* pData = GlobalLock(hGlobal);
        if (!pData) { GlobalFree(hGlobal); ++pos; continue; }
        memcpy(pData, &data[pos], remaining);
        GlobalUnlock(hGlobal);

        ComPtr<IStream> stream;
        if (FAILED(CreateStreamOnHGlobal(hGlobal, TRUE, &stream))) {
            GlobalFree(hGlobal);
            ++pos;
            continue;
        }

        ComPtr<IShellLinkW> link;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr,
                                    CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
            ++pos;
            continue;
        }

        ComPtr<IPersistStream> ps;
        if (FAILED(link.As(&ps))) { ++pos; continue; }

        HRESULT hr = ps->Load(stream.Get());
        if (FAILED(hr)) {
            pos += 4; // Skip past this false positive
            continue;
        }

        // Get stream position to advance our scan past this link
        ULARGE_INTEGER streamPos;
        LARGE_INTEGER zero = {};
        stream->Seek(zero, STREAM_SEEK_CUR, &streamPos);
        DWORD consumed = static_cast<DWORD>(streamPos.QuadPart);
        if (consumed > 20)
            pos += consumed;
        else
            pos += 20;

        JumpListItem item;
        ExtractFromShellLink(link.Get(), item);

        if (item.displayName.empty() && item.path.empty())
            continue;
        if (item.displayName.empty())
            item.displayName = item.path;

        // Avoid duplicates
        bool dup = false;
        for (const auto& existing : results) {
            if (_wcsicmp(existing.path.c_str(), item.path.c_str()) == 0 &&
                existing.arguments == item.arguments) {
                dup = true;
                break;
            }
        }
        if (!dup)
            results.push_back(std::move(item));
    }

    return results;
}

// --------------------------------------------------------------------------
// Find destination files matching a given AUMID or exe path
// --------------------------------------------------------------------------
static std::wstring GetRecentDestDir()
{
    wchar_t appData[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData)))
        return {};
    return std::wstring(appData) + L"\\Microsoft\\Windows\\Recent";
}

// Check if a DestList stream or any link stream in the storage references the exe
static bool StorageMatchesExe(IStorage* stg, const std::wstring& exePath)
{
    // Read DestList stream — it contains path strings in UTF-16
    // We do a substring search for the exe filename
    std::wstring exeFileName;
    {
        const wchar_t* fn = PathFindFileNameW(exePath.c_str());
        if (fn) exeFileName = fn;
        for (auto& c : exeFileName) c = towlower(c);
    }
    if (exeFileName.empty()) return false;

    ComPtr<IStream> destList;
    HRESULT hr = stg->OpenStream(L"DestList", nullptr,
                                  STGM_READ | STGM_SHARE_EXCLUSIVE, 0, &destList);
    if (SUCCEEDED(hr) && destList) {
        // Read up to 64KB of the DestList and search for the exe name in UTF-16
        STATSTG stat = {};
        destList->Stat(&stat, STATFLAG_NONAME);
        DWORD toRead = static_cast<DWORD>(std::min(stat.cbSize.QuadPart, (ULONGLONG)65536));
        if (toRead > 0) {
            std::vector<BYTE> buf(toRead);
            ULONG readBytes = 0;
            if (SUCCEEDED(destList->Read(buf.data(), toRead, &readBytes)) && readBytes > 2) {
                // Search for the exe filename as UTF-16 in the binary data
                std::wstring lower(reinterpret_cast<const wchar_t*>(buf.data()),
                                   readBytes / sizeof(wchar_t));
                for (auto& c : lower) c = towlower(c);
                if (lower.find(exeFileName) != std::wstring::npos)
                    return true;
            }
        }
    }

    // Fallback: check first shell link stream
    ComPtr<IEnumSTATSTG> enumStat;
    hr = stg->EnumElements(0, nullptr, 0, &enumStat);
    if (FAILED(hr)) return false;

    STATSTG st;
    while (enumStat->Next(1, &st, nullptr) == S_OK) {
        if (st.type != STGTY_STREAM || !st.pwcsName ||
            _wcsicmp(st.pwcsName, L"DestList") == 0) {
            if (st.pwcsName) CoTaskMemFree(st.pwcsName);
            continue;
        }

        ComPtr<IStream> stream;
        hr = stg->OpenStream(st.pwcsName, nullptr,
                             STGM_READ | STGM_SHARE_EXCLUSIVE, 0, &stream);
        CoTaskMemFree(st.pwcsName);
        if (FAILED(hr)) continue;

        ComPtr<IShellLinkW> link;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr,
                                    CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
            continue;
        ComPtr<IPersistStream> ps;
        if (FAILED(link.As(&ps))) continue;
        if (FAILED(ps->Load(stream.Get()))) continue;

        wchar_t target[MAX_PATH] = {};
        if (SUCCEEDED(link->GetPath(target, MAX_PATH, nullptr, 0)) && target[0]) {
            const wchar_t* fn = PathFindFileNameW(target);
            if (fn && _wcsicmp(fn, exeFileName.c_str()) == 0)
                return true;
        }
        break; // Only check first link as a quick probe
    }

    return false;
}

// Try to find the AutomaticDestinations-ms file for the given app.
// Note: On Win11, many files are locked (STG_E_LOCKVIOLATION) and the format
// has changed (no numbered shell link streams). This is a best-effort fallback.
static std::wstring FindAutoDestFile(const std::wstring& recentDir, const std::wstring& /*appId*/, const std::wstring& exePath)
{
    std::wstring autoDir = recentDir + L"\\AutomaticDestinations";

    if (exePath.empty()) return {};

    WIN32_FIND_DATAW fd;
    std::wstring pattern = autoDir + L"\\*.automaticDestinations-ms";
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return {};

    std::wstring bestFile;
    int filesChecked = 0;
    constexpr int kMaxFiles = 30;

    do {
        if (++filesChecked > kMaxFiles) break;

        std::wstring filePath = autoDir + L"\\" + fd.cFileName;

        ComPtr<IStorage> stg;
        HRESULT hr = StgOpenStorage(filePath.c_str(), nullptr,
                                    STGM_READ | STGM_SHARE_DENY_WRITE,
                                    nullptr, 0, &stg);
        if (FAILED(hr) || !stg) continue;

        if (StorageMatchesExe(stg.Get(), exePath)) {
            bestFile = filePath;
            break;
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    return bestFile;
}

static std::wstring FindCustomDestFile(const std::wstring& recentDir, const std::wstring& /*appId*/, const std::wstring& exePath)
{
    std::wstring customDir = recentDir + L"\\CustomDestinations";

    if (exePath.empty()) return {};

    // Primary search term: the exe filename (e.g. "firefox.exe")
    std::wstring exeFileName;
    {
        const wchar_t* fn = PathFindFileNameW(exePath.c_str());
        if (fn) exeFileName = fn;
        for (auto& c : exeFileName) c = towlower(c);
    }
    if (exeFileName.empty()) return {};

    // Secondary search term for UWP/Store apps: extract the package name
    // (e.g. "Microsoft.WindowsTerminal" from WindowsApps path)
    std::wstring packageName;
    {
        std::wstring pathLower = exePath;
        for (auto& c : pathLower) c = towlower(c);
        auto waPos = pathLower.find(L"\\windowsapps\\");
        if (waPos != std::wstring::npos) {
            size_t start = waPos + 13; // length of "\\windowsapps\\"
            auto underscore = pathLower.find(L'_', start);
            if (underscore != std::wstring::npos && underscore > start)
                packageName = pathLower.substr(start, underscore - start);
        }
    }

    WIN32_FIND_DATAW fd;
    std::wstring pattern = customDir + L"\\*.customDestinations-ms";
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return {};

    std::wstring bestFile;
    do {
        std::wstring filePath = customDir + L"\\" + fd.cFileName;

        HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) continue;

        BYTE buf[65536];
        DWORD readBytes = 0;
        ReadFile(hFile, buf, sizeof(buf), &readBytes, nullptr);
        CloseHandle(hFile);

        if (readBytes > 4) {
            std::wstring content(reinterpret_cast<const wchar_t*>(buf),
                                 readBytes / sizeof(wchar_t));
            for (auto& c : content) c = towlower(c);

            if (content.find(exeFileName) != std::wstring::npos) {
                bestFile = filePath;
                break;
            }
            // For UWP apps, also search by package name
            if (!packageName.empty() && content.find(packageName) != std::wstring::npos) {
                bestFile = filePath;
                break;
            }
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    return bestFile;
}

// --------------------------------------------------------------------------
// Try IApplicationDocumentLists API (works when AUMID is known)
// --------------------------------------------------------------------------
static std::vector<JumpListItem> TryDocumentListsApi(const std::wstring& appId, int maxItems)
{
    std::vector<JumpListItem> results;

    ComPtr<IApplicationDocumentLists> docLists;
    HRESULT hr = CoCreateInstance(CLSID_AppDocLists, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&docLists));
    if (FAILED(hr) || !docLists)
        return results;

    hr = docLists->SetAppID(appId.c_str());
    if (FAILED(hr))
        return results;

    // Try ADLT_RECENT first, then ADLT_FREQUENT
    for (int listType = 0; listType < 2 && results.empty(); ++listType) {
        APPDOCLISTTYPE adlt = (listType == 0) ? ADLT_RECENT : ADLT_FREQUENT;

        ComPtr<IObjectArray> objArray;
        hr = docLists->GetList(adlt, static_cast<UINT>(maxItems), IID_PPV_ARGS(&objArray));
        if (FAILED(hr) || !objArray)
            continue;

        UINT count = 0;
        objArray->GetCount(&count);

        for (UINT i = 0; i < count && static_cast<int>(results.size()) < maxItems; ++i) {
            JumpListItem item;

            ComPtr<IShellLinkW> link;
            if (SUCCEEDED(objArray->GetAt(i, IID_PPV_ARGS(&link)))) {
                ExtractFromShellLink(link.Get(), item);
            } else {
                ComPtr<IShellItem> shellItem;
                if (SUCCEEDED(objArray->GetAt(i, IID_PPV_ARGS(&shellItem)))) {
                    LPWSTR name = nullptr;
                    if (SUCCEEDED(shellItem->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && name) {
                        item.displayName = name;
                        CoTaskMemFree(name);
                    }
                    LPWSTR path = nullptr;
                    if (SUCCEEDED(shellItem->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                        item.path = path;
                        CoTaskMemFree(path);
                    }
                } else {
                    continue;
                }
            }

            if (item.displayName.empty() && item.path.empty())
                continue;
            if (item.displayName.empty())
                item.displayName = item.path;
            if (item.displayName.empty()) {
                const wchar_t* fname = PathFindFileNameW(item.path.c_str());
                if (fname) item.displayName = fname;
            }

            results.push_back(std::move(item));
        }
    }

    return results;
}

// --------------------------------------------------------------------------
// Explorer special case: enumerate Quick Access pinned folders
// --------------------------------------------------------------------------
static std::vector<JumpListItem> GetExplorerPinnedItems(int maxItems)
{
    std::vector<JumpListItem> results;

    // Quick Access CLSID: {679F85CB-0220-4080-B29B-5540CC05AAB6}
    ComPtr<IShellItem> qaFolder;
    HRESULT hr = SHCreateItemFromParsingName(
        L"shell:::{679F85CB-0220-4080-B29B-5540CC05AAB6}",
        nullptr, IID_PPV_ARGS(&qaFolder));
    if (FAILED(hr) || !qaFolder)
        return results;

    ComPtr<IEnumShellItems> enumItems;
    hr = qaFolder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&enumItems));
    if (FAILED(hr) || !enumItems)
        return results;

    ComPtr<IShellItem> item;
    while (static_cast<int>(results.size()) < maxItems &&
           enumItems->Next(1, &item, nullptr) == S_OK) {
        JumpListItem ji;
        ji.category = L"Pinned";
        ji.isFolder = true;

        LPWSTR name = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && name) {
            ji.displayName = name;
            CoTaskMemFree(name);
        }

        LPWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
            ji.path = path;
            CoTaskMemFree(path);
        } else {
            // For non-filesystem items (like network shares), try URL or parsing name
            if (SUCCEEDED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &path)) && path) {
                ji.path = path;
                CoTaskMemFree(path);
            }
        }

        if (!ji.displayName.empty() && !ji.path.empty())
            results.push_back(std::move(ji));

        item.Reset();
    }

    return results;
}

// --------------------------------------------------------------------------
// Public API
// --------------------------------------------------------------------------
std::vector<JumpListItem> GetJumpListItems(HWND hwnd, const std::wstring& exePath, int maxItems)
{
    std::vector<JumpListItem> results;

    // Special case: Explorer — use Quick Access pinned folders
    if (!exePath.empty()) {
        const wchar_t* fn = PathFindFileNameW(exePath.c_str());
        if (fn && _wcsicmp(fn, L"explorer.exe") == 0) {
            results = GetExplorerPinnedItems(maxItems);
            return results;
        }
    }

    std::wstring recentDir = GetRecentDestDir();

    // 1. Try to get AUMID from the window property store
    std::wstring appId = GetAumidFromWindow(hwnd);

    // 2. If we have an AUMID, try the IApplicationDocumentLists API first (fast)
    if (!appId.empty())
        results = TryDocumentListsApi(appId, maxItems);

    // 3. Read destination files directly (most reliable on modern Windows)
    if (results.empty() && !recentDir.empty()) {
        // Try CustomDestinations first (app-defined jump lists — most common on Win10/11)
        std::wstring customFile = FindCustomDestFile(recentDir, appId, exePath);
        if (!customFile.empty())
            results = ReadCustomDestinations(customFile, maxItems);

        // If still empty, try AutomaticDestinations (recent docs — classic format)
        if (results.empty()) {
            std::wstring autoFile = FindAutoDestFile(recentDir, appId, exePath);
            if (!autoFile.empty())
                results = ReadAutoDestinations(autoFile, maxItems);
        }
    }

    // 4. If direct file search failed and we had no AUMID,
    //    try finding one from a Start Menu shortcut and retry the API
    if (results.empty() && appId.empty() && !exePath.empty()) {
        std::wstring lnkPath = FindShortcutForExe(exePath);
        if (!lnkPath.empty()) {
            appId = GetAumidFromShortcut(lnkPath);
            if (!appId.empty())
                results = TryDocumentListsApi(appId, maxItems);
        }
    }

    // Set default category for items without one
    for (auto& item : results) {
        if (item.category.empty())
            item.category = L"Tasks";
    }

    return results;
}
