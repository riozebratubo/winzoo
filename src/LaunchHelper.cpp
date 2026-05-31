#include "LaunchHelper.h"
#include <shellapi.h>
#include <memory>
#include <vector>

void RegisterLaunchHelperClass(HINSTANCE /*hInst*/) {}

// ─────────────────────────────────────────────────────────────────────────────
// Background thread: wait for the launched app to create its main window,
// then move it to the target monitor.
// ─────────────────────────────────────────────────────────────────────────────

struct LaunchMoveCtx {
    HANDLE            hProc;     // launched process handle (may be nullptr)
    HMONITOR          hMon;      // target monitor
    std::vector<HWND> snapshot;  // visible top-level windows before launch
    DWORD             pid;       // PID of hProc (0 if unknown)
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

    if (!best) return 0;
    if (MonitorFromWindow(best, MONITOR_DEFAULTTONEAREST) == hMon) return 0;

    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfo(hMon, &mi)) return 0;

    RECT wr;
    GetWindowRect(best, &wr);
    int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    int x  = (mi.rcWork.left + mi.rcWork.right  - ww) / 2;
    int y  = (mi.rcWork.top  + mi.rcWork.bottom - wh) / 2;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top)  y = mi.rcWork.top;

    WINDOWPLACEMENT wp = {};
    wp.length = sizeof(wp);
    GetWindowPlacement(best, &wp);
    if (wp.showCmd == SW_SHOWMAXIMIZED) {
        ShowWindow(best, SW_RESTORE);
        SetWindowPos(best, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(best, SW_MAXIMIZE);
    } else {
        SetWindowPos(best, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

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
    auto ctxOwner = std::make_unique<LaunchMoveCtx>(LaunchMoveCtx{sei.hProcess, hMon, std::move(snapshot), pid});

    HANDLE hThread = CreateThread(nullptr, 0, MoveToMonitorThread, ctxOwner.get(), 0, nullptr);
    if (hThread) {
        ctxOwner.release();  // ownership transferred to thread
        CloseHandle(hThread);
    } else {
        if (ctxOwner->hProc) CloseHandle(ctxOwner->hProc);
    }
}

