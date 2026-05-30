#pragma once
#include <windows.h>
#include <memory>
#include <vector>
#include "Settings.h"
#include "TaskbarWindow.h"

class App {
public:
    static App& Instance();

    int  Run(HINSTANCE hInst, int nCmdShow);
    void Quit();
    void RequestRestart();
    void PropagateSettings(const Settings& newSettings, TaskbarWindow* origin);

private:
    App()  = default;
    ~App() = default;
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    bool Init(HINSTANCE hInst);
    void Shutdown();

    HINSTANCE     hInst_    = nullptr;
    HANDLE        mutex_    = nullptr;
    std::vector<std::unique_ptr<TaskbarWindow>> taskbars_;
    Settings      settings_;
    bool          restart_  = false;
};
