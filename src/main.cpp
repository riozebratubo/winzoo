#include <windows.h>
#include "App.h"
#ifdef WINZOO_CRASH_HANDLER
#include "CrashHandler.h"
#endif

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
#ifdef WINZOO_CRASH_HANDLER
    // Install before anything else so even early-startup faults are reported.
    CrashHandler::Install();
#endif
    return App::Instance().Run(hInstance, nCmdShow);
}
