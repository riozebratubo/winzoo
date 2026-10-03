#pragma once
#include <windows.h>
#include <string>

// Long-path-safe process/module path queries. The manifest declares
// longPathAware, so paths can exceed MAX_PATH; these helpers size their
// buffers to the NT path limit (32767 chars) instead of silently truncating.
// All return an empty string on failure.

inline std::wstring GetProcessImagePath(HANDLE hProc)
{
    if (!hProc) return {};
    std::wstring buf(32767, L'\0');
    DWORD len = static_cast<DWORD>(buf.size());
    if (!QueryFullProcessImageNameW(hProc, 0, buf.data(), &len))
        return {};
    buf.resize(len);
    return buf;
}

// Image path of the process owning `hwnd`.
inline std::wstring GetWindowProcessPath(HWND hwnd)
{
    if (!hwnd) return {};
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return {};
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return {};
    std::wstring path = GetProcessImagePath(hProc);
    CloseHandle(hProc);
    return path;
}

// Full path of this process's own executable.
inline std::wstring GetOwnExePath()
{
    std::wstring buf(32767, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (!n || n >= buf.size()) return {};
    buf.resize(n);
    return buf;
}
