#include "App.h"
#include "Registry.h"

App& App::Instance()
{
    static App instance;
    return instance;
}

int App::Run(HINSTANCE hInst, int /*nCmdShow*/)
{
    if (!Init(hInst))
        return 1;

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Shutdown();
    return static_cast<int>(msg.wParam);
}

bool App::Init(HINSTANCE hInst)
{
    hInst_ = hInst;

    mutex_ = CreateMutexW(nullptr, TRUE, L"WinzooSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex_);
        mutex_ = nullptr;
        return false;
    }

    settings_ = LoadSettings();

    // Hide the native Windows taskbar
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray) ShowWindow(tray, SW_HIDE);

    // Hide secondary trays (multi-monitor)
    HWND sec = nullptr;
    while ((sec = FindWindowExW(nullptr, sec, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr)
        ShowWindow(sec, SW_HIDE);

    if (!taskbar_.Create(hInst_, settings_))
        return false;

    taskbar_.Show();
    return true;
}

void App::Shutdown()
{
    taskbar_.Destroy();

    // Restore the native Windows taskbar
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray) ShowWindow(tray, SW_SHOW);

    HWND sec = nullptr;
    while ((sec = FindWindowExW(nullptr, sec, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr)
        ShowWindow(sec, SW_SHOW);

    if (mutex_) {
        ReleaseMutex(mutex_);
        CloseHandle(mutex_);
        mutex_ = nullptr;
    }
}

void App::Quit()
{
    PostQuitMessage(0);
}
