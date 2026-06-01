// winzoo_hook.dll — WH_CALLWNDPROC global hook to intercept ITaskbarList3
// progress notifications sent to Shell_TrayWnd and relay them to Winzoo.
//
// Message ID discovery:
//   Build in debug mode (define WINZOO_HOOK_LOG) to log all messages received
//   by Shell_TrayWnd to OutputDebugStringA. Run DebugView to capture them while
//   triggering a file copy in Explorer or any app that calls SetProgressValue.
//   The two message IDs of interest are for SetProgressState and SetProgressValue.
//   Known candidates (from Windows shell research — verify with logging):
//     kMsgProgressState: WM_USER offset sent with TBPFLAG in lParam, appHwnd in wParam
//     kMsgProgressValue: WM_USER offset sent with progress data, appHwnd in wParam
//   Update these constants once confirmed.
//
// Message format relayed to Winzoo (registered "WinzooProgress" message):
//   wParam = app HWND
//   lParam = MAKELPARAM(state, percent)  where state=TBPF_* and percent=0-100

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>

// ─── Known ITaskbarList3 → Shell_TrayWnd private message IDs ────────────────
//
// These are undocumented Windows internal messages. Values below are PLACEHOLDERS
// that need to be confirmed via logging (see top-of-file instructions).
//
// To discover: enable WINZOO_HOOK_LOG below, rebuild, run Winzoo, trigger a
// Windows file copy operation, and check DebugView / OutputDebugString output
// for messages arriving at Shell_TrayWnd. The two distinct message IDs that
// correlate with SetProgressState / SetProgressValue calls are the ones to use.
//
// Confirmed values for Windows 10/11 (update after testing):
static constexpr UINT kMsgProgressState = 0;  // TODO: fill in after discovery
static constexpr UINT kMsgProgressValue = 0;  // TODO: fill in after discovery

// Enable to log all messages to Shell_TrayWnd via OutputDebugStringA.
// #define WINZOO_HOOK_LOG

// ─── Shared hook state ───────────────────────────────────────────────────────

// DLL-instance-local state (per process). FindWindow/RegisterWindowMessage are
// used at first call rather than shared memory, because they work cross-process.
static HHOOK   g_hook     = nullptr;
static HWND    g_winzoo   = nullptr;  // cached Winzoo HWND (refreshed if stale)
static UINT    g_relayMsg = 0;
static UINT    g_relayMsgParam = 0;   // the relayMsg passed from Winzoo at install

// ─── Hook procedure ──────────────────────────────────────────────────────────

static LRESULT CALLBACK CallWndHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0) {
        auto* info = reinterpret_cast<CWPSTRUCT*>(lParam);

        // Lazily find Shell_TrayWnd — cache it (it rarely changes).
        static HWND hTray = nullptr;
        if (!hTray) hTray = FindWindowW(L"Shell_TrayWnd", nullptr);

        if (hTray && info->hwnd == hTray) {
#ifdef WINZOO_HOOK_LOG
            char buf[128];
            wsprintfA(buf, "[winzoo_hook] Shell_TrayWnd msg=0x%04X wp=%p lp=%p\n",
                      info->message, (void*)info->wParam, (void*)info->lParam);
            OutputDebugStringA(buf);
#endif
            // Check if this is a known progress message.
            bool isState = (kMsgProgressState != 0 && info->message == kMsgProgressState);
            bool isValue = (kMsgProgressValue != 0 && info->message == kMsgProgressValue);

            if ((isState || isValue) && g_relayMsgParam != 0) {
                // Re-find Winzoo HWND if stale.
                if (!g_winzoo || !IsWindow(g_winzoo))
                    g_winzoo = FindWindowW(L"WinzooTaskbar", nullptr);

                if (g_winzoo) {
                    HWND appHwnd = reinterpret_cast<HWND>(info->wParam);
                    int  state   = 0;
                    int  percent = 0;

                    if (isState) {
                        // SetProgressState: lParam = TBPFLAG
                        state   = static_cast<int>(info->lParam);
                        percent = 0;  // state-only update; percent unchanged
                    } else {
                        // SetProgressValue: decode value/total from lParam.
                        // Format TBD — update once message layout is confirmed.
                        // Tentative: HIWORD(lParam)=value, LOWORD(lParam)=total (16-bit each)
                        // OR: lParam might be a pointer to a struct (if SendMessage uses WM_COPYDATA)
                        ULONGLONG completed = static_cast<ULONGLONG>(HIWORD(info->lParam));
                        ULONGLONG total     = static_cast<ULONGLONG>(LOWORD(info->lParam));
                        if (total > 0)
                            percent = static_cast<int>(completed * 100 / total);
                        state = 2;  // kTBPF_NORMAL
                    }

                    // Clamp percent
                    if (percent < 0)   percent = 0;
                    if (percent > 100) percent = 100;

                    PostMessageW(g_winzoo, g_relayMsgParam,
                                 reinterpret_cast<WPARAM>(appHwnd),
                                 MAKELPARAM(static_cast<WORD>(state),
                                            static_cast<WORD>(percent)));
                }
            }
        }
    }
    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

// ─── Exports ─────────────────────────────────────────────────────────────────

extern "C" __declspec(dllexport)
BOOL WINAPI InstallHook(HWND winzooHwnd, UINT relayMsg)
{
    if (g_hook) return TRUE;  // already installed

    g_relayMsgParam = relayMsg;
    g_winzoo        = winzooHwnd;

    g_hook = SetWindowsHookExW(WH_CALLWNDPROC, CallWndHookProc,
                                GetModuleHandleW(nullptr), 0);
    return g_hook != nullptr;
}

extern "C" __declspec(dllexport)
void WINAPI UninstallHook()
{
    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }
}

BOOL WINAPI DllMain(HINSTANCE /*hInst*/, DWORD reason, LPVOID /*reserved*/)
{
    if (reason == DLL_PROCESS_DETACH && g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }
    return TRUE;
}
