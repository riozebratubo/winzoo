#pragma once
#include <windows.h>
#include "Settings.h"
#include "TaskbarWindow.h"

class App {
public:
    static App& Instance();

    int  Run(HINSTANCE hInst, int nCmdShow);
    void Quit();

private:
    App()  = default;
    ~App() = default;
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    bool Init(HINSTANCE hInst);
    void Shutdown();

    HINSTANCE     hInst_   = nullptr;
    HANDLE        mutex_   = nullptr;
    TaskbarWindow taskbar_;
    Settings      settings_;
};
