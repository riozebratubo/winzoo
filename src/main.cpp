#include <windows.h>
#include "App.h"
#ifdef WINZOO_CRASH_HANDLER
#include "CrashHandler.h"
#endif

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    // De-elevated tray re-broadcast helper. The main (elevated) winzoo relaunches
    // itself at medium integrity with this flag ~1.5s after startup (see
    // TaskbarWindow's kTimerTrayReregister). Only a TaskbarCreated broadcast from a
    // process at/below the tray apps' integrity level makes them re-register — one
    // from elevated winzoo or from inside Explorer does NOT (verified). Without this
    // the tray trickles in over ~30s after the startup Explorer restart; with it the
    // stragglers re-register at once. Handle the flag BEFORE App::Run so this helper
    // never touches the single-instance mutex — it just broadcasts and exits.
    if (wcsstr(GetCommandLineW(), L"--rebroadcast-tray")) {
        if (UINT msg = RegisterWindowMessageW(L"TaskbarCreated")) {
            DWORD_PTR res = 0;
            SendMessageTimeoutW(HWND_BROADCAST, msg, 0, 0, SMTO_ABORTIFHUNG, 3000, &res);
        }
        return 0;
    }

#ifdef WINZOO_CRASH_HANDLER
    // Install before anything else so even early-startup faults are reported.
    CrashHandler::Install();
#endif
    return App::Instance().Run(hInstance, nCmdShow);
}
