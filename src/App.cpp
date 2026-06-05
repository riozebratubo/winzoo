#include "App.h"
#include "Registry.h"
#include "SettingsFile.h"
#include "AppMenuWindow.h"
#include "LaunchHelper.h"
#include "TaskbarRelocate.h"
#include <objbase.h>

struct MonitorEnumData {
    std::vector<HMONITOR>* monitors;
    HMONITOR               primary;
};

static BOOL CALLBACK CollectMonitors(HMONITOR hMon, HDC, LPRECT, LPARAM lParam)
{
    auto* d = reinterpret_cast<MonitorEnumData*>(lParam);
    d->monitors->push_back(hMon);

    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfo(hMon, &mi);
    if (mi.dwFlags & MONITORINFOF_PRIMARY)
        d->primary = hMon;

    return TRUE;
}

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

    if (restart_) {
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        ShellExecuteW(nullptr, L"open", exePath, nullptr, nullptr, SW_SHOWNORMAL);
    }

    return static_cast<int>(msg.wParam);
}

bool App::Init(HINSTANCE hInst)
{
    hInst_ = hInst;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    mutex_ = CreateMutexW(nullptr, TRUE, L"WinzooSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex_);
        mutex_ = nullptr;
        CoUninitialize();
        return false;
    }

    settings_ = LoadSettings();

    // If a settings file was placed next to the exe, absorb it and re-normalise.
    if (ImportAndDeleteSettingsFile(settings_)) {
        SaveSettings(settings_);
        settings_ = LoadSettings(); // re-normalise through registry clamping
    }

    AppMenuWindow::CachePowerOptions();
    RegisterLaunchHelperClass(hInst);

    // Method B: dock Explorer's taskbar on winzoo's edge so ApplicationFrameWindow
    // apps (Task Manager etc.) minimize toward winzoo. Runs before we hide/probe the
    // tray, and only restarts Explorer when the edge actually needs to change.
    if (settings_.taskbarHookMethod == TaskbarHookMethod::BeforeExplorer)
        RelocateExplorerTaskbarToMatch(settings_.position);

    // Hide the native Windows taskbar
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray) ShowWindow(tray, SW_HIDE);

    // Hide secondary trays (multi-monitor)
    HWND sec = nullptr;
    while ((sec = FindWindowExW(nullptr, sec, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr)
        ShowWindow(sec, SW_HIDE);

    // Enumerate monitors
    std::vector<HMONITOR> monitors;
    MonitorEnumData enumData{ &monitors, nullptr };
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitors,
                        reinterpret_cast<LPARAM>(&enumData));

    HMONITOR primaryMon = enumData.primary;
    if (!primaryMon && !monitors.empty())
        primaryMon = monitors[0];

    bool allMonitors = (settings_.taskbarMonitorMode == TaskbarMonitorMode::AllMonitors)
                    && (settings_.position != TaskbarPosition::Floating);

    if (allMonitors) {
        for (HMONITOR hMon : monitors) {
            bool isPrimary = (hMon == primaryMon);
            auto tb = std::make_unique<TaskbarWindow>();
            if (!tb->Create(hInst_, settings_, hMon, isPrimary)) {
                Shutdown();
                return false;
            }
            tb->Show();
            taskbars_.push_back(std::move(tb));
        }
    } else {
        // Primary monitor only
        auto tb = std::make_unique<TaskbarWindow>();
        if (!tb->Create(hInst_, settings_, primaryMon, true)) {
            Shutdown();
            return false;
        }
        tb->Show();
        taskbars_.push_back(std::move(tb));
    }

    return true;
}

void App::Shutdown()
{
    for (auto& tb : taskbars_)
        tb->Destroy();
    taskbars_.clear();

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

    CoUninitialize();
}

void App::PropagateSettings(const Settings& newSettings, TaskbarWindow* origin)
{
    bool monitorModeChanged = (newSettings.taskbarMonitorMode != settings_.taskbarMonitorMode);
    // A hook-method change needs a winzoo restart so Init() re-runs the Explorer-
    // taskbar relocation logic. (A later position change with Method B active simply
    // self-heals on the next launch, when Init() re-aligns Explorer's edge.)
    bool hookMethodChanged  = (newSettings.taskbarHookMethod != settings_.taskbarHookMethod);
    settings_ = newSettings;

    for (auto& tb : taskbars_) {
        if (tb.get() != origin)
            tb->ApplySettings(settings_);
    }

    if (monitorModeChanged || hookMethodChanged)
        RequestRestart();
}

void App::Quit()
{
    PostQuitMessage(0);
}

void App::RequestRestart()
{
    restart_ = true;
    PostQuitMessage(0);
}
