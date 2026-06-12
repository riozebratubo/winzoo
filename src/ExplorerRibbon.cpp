#include "ExplorerRibbon.h"
#include "TaskbarRelocate.h"   // RestartExplorer()
#include <windows.h>
#include <string>

namespace {

// The two shell view-adapter CLSIDs we suppress. Redirecting their InProcServer32 to a
// non-existent DLL (note the trailing underscore on the filename) makes the Windows 11
// command-bar adapter fail to instantiate, so File Explorer renders the Windows 10 ribbon
// instead. These GUIDs are not normally present under HKCU\Software\Classes, so creating
// and deleting the whole key there is safe and self-contained.
constexpr const wchar_t* kClsidKeys[] = {
    L"Software\\Classes\\CLSID\\{2aa9162e-c906-4dd9-ad0b-3d24a8eef5a0}",  // ItemsViewAdapter
    L"Software\\Classes\\CLSID\\{6480100b-5a83-4d1e-9f69-8ae5a88e9a33}",  // Xaml Island View Adapter
};

// "<System32>\Windows.UI.FileExplorer.dll_" — a real DLL name with a trailing underscore
// so the path never resolves. The underscore (not a missing file) is the documented trick.
std::wstring SentinelDllPath()
{
    wchar_t sys[MAX_PATH] = {};
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring p = (n && n < MAX_PATH) ? std::wstring(sys, n)
                                         : std::wstring(L"C:\\Windows\\System32");
    p += L"\\Windows.UI.FileExplorer.dll_";
    return p;
}

std::wstring InProcKey(const wchar_t* clsidKey)
{
    return std::wstring(clsidKey) + L"\\InProcServer32";
}

}  // namespace

namespace ExplorerRibbon {

bool IsEnabled()
{
    // Enabled iff the first redirect's InProcServer32 default equals our sentinel path.
    const std::wstring key = InProcKey(kClsidKeys[0]);
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return false;

    wchar_t buf[MAX_PATH + 8] = {};
    DWORD cb = sizeof(buf), type = 0;
    LSTATUS rs = RegQueryValueExW(hKey, nullptr, nullptr, &type,
                                  reinterpret_cast<BYTE*>(buf), &cb);
    RegCloseKey(hKey);
    if (rs != ERROR_SUCCESS || type != REG_SZ) return false;
    return lstrcmpiW(buf, SentinelDllPath().c_str()) == 0;
}

bool SetEnabled(bool on)
{
    bool allOk = true;

    if (on) {
        const std::wstring dll = SentinelDllPath();
        for (const wchar_t* clsidKey : kClsidKeys) {
            const std::wstring key = InProcKey(clsidKey);
            HKEY hKey = nullptr;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0,
                                KEY_SET_VALUE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
                allOk = false;
                continue;
            }
            if (RegSetValueExW(hKey, nullptr, 0, REG_SZ,
                               reinterpret_cast<const BYTE*>(dll.c_str()),
                               static_cast<DWORD>((dll.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS)
                allOk = false;
            static const wchar_t kApartment[] = L"Apartment";
            RegSetValueExW(hKey, L"ThreadingModel", 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(kApartment),
                           static_cast<DWORD>(sizeof(kApartment)));
            RegCloseKey(hKey);
        }
    } else {
        for (const wchar_t* clsidKey : kClsidKeys) {
            LSTATUS rs = RegDeleteTreeW(HKEY_CURRENT_USER, clsidKey);
            if (rs != ERROR_SUCCESS && rs != ERROR_FILE_NOT_FOUND)
                allOk = false;
        }
    }

    return allOk;
}

void Apply(bool on)
{
    SetEnabled(on);
    RestartExplorer();  // Explorer reads the view-adapter CLSIDs only at startup.
}

}  // namespace ExplorerRibbon
