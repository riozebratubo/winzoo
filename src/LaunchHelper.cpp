#include "LaunchHelper.h"
#include <shellapi.h>
#include <psapi.h>
#include <memory>
#include <vector>

void RegisterLaunchHelperClass(HINSTANCE /*hInst*/) {}

static void MoveWindowToMonitor(HWND hwnd, HMONITOR hMon);
static bool IsRealAppWindow(HWND h);
static HWND FindWindowByProcessName(const wchar_t* exeName);

// ─────────────────────────────────────────────────────────────────────────────
// Background thread: wait for the launched app to create its main window,
// then move it to the target monitor.
// ─────────────────────────────────────────────────────────────────────────────

struct LaunchMoveCtx {
    HANDLE            hProc;     // launched process handle (may be nullptr)
    HMONITOR          hMon;      // target monitor
    std::vector<HWND> snapshot;  // visible top-level windows before launch
    DWORD             pid;       // PID of hProc (0 if unknown)
    wchar_t           exeName[64]; // exe filename for fallback detection
};

// Score a candidate window; higher = more likely to be the real app window.
static int ScoreCandidate(HWND h, const std::vector<HWND>& snapshot, DWORD preferPid)
{
    for (HWND s : snapshot) if (s == h) return 0;  // existed before launch
    if (!IsWindowVisible(h)) return 0;
    if (GetWindow(h, GW_OWNER) != nullptr) return 0;
    if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return 0;

    RECT r;
    GetWindowRect(h, &r);
    int w = r.right - r.left, ht = r.bottom - r.top;
    if (w < 100 || ht < 50) return 0;

    int score = w * ht;
    if (preferPid) {
        DWORD wPid = 0;
        GetWindowThreadProcessId(h, &wPid);
        if (wPid == preferPid) score += 100000000;
    }
    return score;
}

static DWORD WINAPI MoveToMonitorThread(LPVOID pv)
{
    auto ctx = std::unique_ptr<LaunchMoveCtx>(static_cast<LaunchMoveCtx*>(pv));
    HANDLE            hProc    = ctx->hProc;
    HMONITOR          hMon     = ctx->hMon;
    std::vector<HWND> snapshot = std::move(ctx->snapshot);
    DWORD             pid      = ctx->pid;
    wchar_t           exeName[64];
    wcsncpy_s(exeName, ctx->exeName, _TRUNCATE);
    ctx.reset();

    // Poll every 10 ms for up to 5 s.  Polling at 10 ms means we catch the
    // window within one screen refresh (~16 ms at 60 Hz), which is effectively
    // invisible.  We do NOT call WaitForInputIdle first — that can block for
    // 200 ms+ while the window is already visible at the wrong position.
    HWND best = nullptr;
    for (int attempt = 0; attempt < 500 && !best; ++attempt) {
        Sleep(10);
        struct Scan { const std::vector<HWND>* snap; DWORD pid; HWND best; int bestScore; };
        Scan s{ &snapshot, pid, nullptr, 0 };
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            auto& sc = *reinterpret_cast<Scan*>(lp);
            int score = ScoreCandidate(h, *sc.snap, sc.pid);
            if (score > sc.bestScore) { sc.bestScore = score; sc.best = h; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&s));
        best = s.best;
    }

    if (hProc) CloseHandle(hProc);

    // Fallback: the app might be a singleton that reused an existing window
    // (e.g. Task Manager). Find it by process name.
    if (!best && exeName[0])
        best = FindWindowByProcessName(exeName);

    // Last resort: if the foreground changed to a real app window, use that.
    if (!best) {
        HWND fg = GetForegroundWindow();
        if (fg && IsRealAppWindow(fg)) best = fg;
    }

    if (!best) return 0;

    MoveWindowToMonitor(best, hMon);
    return 0;
}

void LaunchOnMonitor(HINSTANCE /*hInst*/, HMONITOR hMon,
                     const wchar_t* exe, const wchar_t* args, int nShow)
{
    // Snapshot all currently visible top-level windows so we can identify
    // the new ones that appear after the launch.
    std::vector<HWND> snapshot;
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        if (IsWindowVisible(h))
            reinterpret_cast<std::vector<HWND>*>(lp)->push_back(h);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&snapshot));

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb       = L"open";
    sei.lpFile       = exe;
    sei.lpParameters = (args && *args) ? args : nullptr;
    sei.nShow        = nShow;

    if (!ShellExecuteExW(&sei)) return;

    DWORD pid = sei.hProcess ? GetProcessId(sei.hProcess) : 0;
    auto ctxOwner = std::make_unique<LaunchMoveCtx>();
    ctxOwner->hProc = sei.hProcess;
    ctxOwner->hMon  = hMon;
    ctxOwner->snapshot = std::move(snapshot);
    ctxOwner->pid   = pid;

    // Store exe filename for fallback singleton detection.
    const wchar_t* fname = wcsrchr(exe, L'\\');
    fname = fname ? fname + 1 : exe;
    wcsncpy_s(ctxOwner->exeName, fname, _TRUNCATE);

    HANDLE hThread = CreateThread(nullptr, 0, MoveToMonitorThread, ctxOwner.get(), 0, nullptr);
    if (hThread) {
        ctxOwner.release();  // ownership transferred to thread
        CloseHandle(hThread);
    } else {
        if (ctxOwner->hProc) CloseHandle(ctxOwner->hProc);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Foreground-tracking variant: works for singleton apps (e.g. Task Manager)
// that reuse an existing window instead of creating a new one.
// ─────────────────────────────────────────────────────────────────────────────

static void MoveWindowToMonitor(HWND hwnd, HMONITOR hMon)
{
    if (!hwnd || !hMon) return;
    if (MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) == hMon) return;

    for (int retry = 0; retry < 5; ++retry) {
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfo(hMon, &mi)) return;

        RECT wr;
        GetWindowRect(hwnd, &wr);
        int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
        int x  = (mi.rcWork.left + mi.rcWork.right  - ww) / 2;
        int y  = (mi.rcWork.top  + mi.rcWork.bottom - wh) / 2;
        if (x < mi.rcWork.left) x = mi.rcWork.left;
        if (y < mi.rcWork.top)  y = mi.rcWork.top;

        WINDOWPLACEMENT wp = {};
        wp.length = sizeof(wp);
        GetWindowPlacement(hwnd, &wp);
        if (wp.showCmd == SW_SHOWMAXIMIZED) {
            ShowWindow(hwnd, SW_RESTORE);
            SetWindowPos(hwnd, nullptr, x, y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            ShowWindow(hwnd, SW_MAXIMIZE);
        } else {
            SetWindowPos(hwnd, nullptr, x, y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }

        // Verify the move stuck; some apps restore their saved position.
        Sleep(150);
        if (MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) == hMon) return;
    }
}

// Check if a window is a real app window (not desktop, shell, or tool window).
static bool IsRealAppWindow(HWND h)
{
    if (!h || !IsWindowVisible(h)) return false;
    if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return false;
    if (GetWindow(h, GW_OWNER) != nullptr) return false;

    // Reject desktop/shell windows.
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (_wcsicmp(cls, L"Progman") == 0) return false;
    if (_wcsicmp(cls, L"WorkerW") == 0) return false;

    RECT r;
    GetWindowRect(h, &r);
    if ((r.right - r.left) < 200 || (r.bottom - r.top) < 100) return false;

    return true;
}

// Find a visible top-level window whose owning process exe matches `exeName`
// (case-insensitive, filename only).
static HWND FindWindowByProcessName(const wchar_t* exeName)
{
    struct Ctx { const wchar_t* name; HWND found; };
    Ctx ctx{ exeName, nullptr };

    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto& c = *reinterpret_cast<Ctx*>(lp);
        if (!IsRealAppWindow(h)) return TRUE;

        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (!pid) return TRUE;

        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hProc) return TRUE;

        wchar_t path[MAX_PATH] = {};
        DWORD sz = MAX_PATH;
        BOOL ok = QueryFullProcessImageNameW(hProc, 0, path, &sz);
        CloseHandle(hProc);

        if (ok) {
            // Extract filename from path.
            const wchar_t* fname = wcsrchr(path, L'\\');
            fname = fname ? fname + 1 : path;
            if (_wcsicmp(fname, c.name) == 0) {
                c.found = h;
                return FALSE;  // stop enumeration
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    return ctx.found;
}

struct ActivateMoveCtx {
    HWND     hwndCaller;   // our taskbar window (to ignore)
    HWND     hwndBefore;   // foreground before launch
    HMONITOR hMon;         // target monitor
    wchar_t  exeName[64];  // e.g. "taskmgr.exe"
};

static DWORD WINAPI WaitAndMoveThread(LPVOID pv)
{
    auto ctx = std::unique_ptr<ActivateMoveCtx>(static_cast<ActivateMoveCtx*>(pv));

    // Phase 1: Wait up to 3s for the foreground to change to a valid app window.
    // Skip our taskbar (hwndCaller) but NOT hwndBefore — the launched app might
    // have already been foreground.
    for (int i = 0; i < 300; ++i) {
        Sleep(10);
        HWND fg = GetForegroundWindow();
        if (!fg) continue;
        if (fg == ctx->hwndCaller) continue;
        if (fg == ctx->hwndBefore) continue;
        if (!IsRealAppWindow(fg)) continue;
        // Foreground changed to a real app window — this is our target.
        MoveWindowToMonitor(fg, ctx->hMon);
        return 0;
    }

    // Phase 2: Foreground didn't change. The app was likely already foreground
    // (singleton case). Find it by process name and move it.
    HWND target = FindWindowByProcessName(ctx->exeName);
    if (target) {
        MoveWindowToMonitor(target, ctx->hMon);
        return 0;
    }

    // Phase 3: Process-name scan failed (e.g. WinUI app hosted in
    // ApplicationFrameHost). Last resort: move whatever is currently foreground
    // if it's a valid app window and not our taskbar.
    HWND fg = GetForegroundWindow();
    if (fg && fg != ctx->hwndCaller && IsRealAppWindow(fg))
        MoveWindowToMonitor(fg, ctx->hMon);

    return 0;
}

void LaunchOrActivateOnMonitor(HWND hwndCaller, HMONITOR hMon,
                               const wchar_t* exe, const wchar_t* args, int nShow)
{
    if (!hMon) return;

    HWND fgBefore = GetForegroundWindow();

    ShellExecuteW(hwndCaller, L"open", exe, (args && *args) ? args : nullptr, nullptr, nShow);

    // Extract filename from exe path for Phase 2 process-name matching.
    const wchar_t* fname = wcsrchr(exe, L'\\');
    fname = fname ? fname + 1 : exe;

    auto ctx = std::make_unique<ActivateMoveCtx>();
    ctx->hwndCaller = hwndCaller;
    ctx->hwndBefore = fgBefore;
    ctx->hMon       = hMon;
    wcsncpy_s(ctx->exeName, fname, _TRUNCATE);

    HANDLE hThread = CreateThread(nullptr, 0, WaitAndMoveThread, ctx.get(), 0, nullptr);
    if (hThread) {
        ctx.release();
        CloseHandle(hThread);
    }
}
