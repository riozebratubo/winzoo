#include "TaskbarProxy.h"
#include <string>

using InstallHookFn   = BOOL(WINAPI*)(HWND winzooHwnd, UINT relayMsg);
using UninstallHookFn = void(WINAPI*)();

bool TaskbarProxy::Install(HWND winzooHwnd)
{
    relayMsg_ = RegisterWindowMessageW(L"WinzooProgress");
    if (!relayMsg_) return false;

    // Load the hook DLL from the same directory as the executable.
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* last = wcsrchr(exePath, L'\\');
    if (last) *(last + 1) = L'\0';
    std::wstring dllPath = std::wstring(exePath) + L"winzoo_hook.dll";

    hookDll_ = LoadLibraryW(dllPath.c_str());
    if (!hookDll_) return false;

    auto fnInstall = reinterpret_cast<InstallHookFn>(
        GetProcAddress(hookDll_, "InstallHook"));
    if (!fnInstall) {
        FreeLibrary(hookDll_);
        hookDll_ = nullptr;
        return false;
    }

    if (!fnInstall(winzooHwnd, relayMsg_)) {
        FreeLibrary(hookDll_);
        hookDll_ = nullptr;
        return false;
    }

    // After installing the hook, broadcast TaskbarCreated so running apps
    // reinitialize their ITaskbarList3 objects and resend current progress state.
    UINT taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    if (taskbarCreatedMsg)
        PostMessageW(HWND_BROADCAST, taskbarCreatedMsg, 0, 0);

    return true;
}

void TaskbarProxy::Uninstall()
{
    if (hookDll_) {
        auto fnUninstall = reinterpret_cast<UninstallHookFn>(
            GetProcAddress(hookDll_, "UninstallHook"));
        if (fnUninstall) fnUninstall();
        FreeLibrary(hookDll_);
        hookDll_ = nullptr;
    }
    relayMsg_ = 0;
}
