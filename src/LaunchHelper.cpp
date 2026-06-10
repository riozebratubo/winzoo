#include "LaunchHelper.h"
#include <shellapi.h>
#include <psapi.h>
#include <shlobj.h>
#include <shldisp.h>
#include <exdisp.h>
#include <servprov.h>
#include <memory>
#include <vector>

void RegisterLaunchHelperClass(HINSTANCE /*hInst*/) {}

static void MoveWindowToMonitor(HWND hwnd, HMONITOR hMon);
static void ForceForegroundWindow(HWND hwnd);
static bool IsRealAppWindow(HWND h);
static HWND FindWindowByProcessName(const wchar_t* exeName);
static bool ForegroundIsTarget(HWND fg, const wchar_t* exeName);

// ─────────────────────────────────────────────────────────────────────────────
// De-elevated launching.
//
// winzoo's manifest requests highestAvailable, so for an admin user the process
// runs elevated. A direct ShellExecute from an elevated process starts the child
// elevated too — we do NOT want every app launched from the taskbar to run as
// administrator. The fix is to ask Explorer (which runs at medium integrity) to
// perform the launch on our behalf, so the child inherits Explorer's token.
// ─────────────────────────────────────────────────────────────────────────────

static bool IsProcessElevated()
{
    HANDLE hTok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hTok)) return false;
    TOKEN_ELEVATION elev{};
    DWORD cb = sizeof(elev);
    bool elevated = false;
    if (GetTokenInformation(hTok, TokenElevation, &elev, sizeof(elev), &cb))
        elevated = elev.TokenIsElevated != 0;
    CloseHandle(hTok);
    return elevated;
}

// Obtain Explorer's own IShellDispatch2 automation object by walking the desktop
// shell view (ShellWindows → desktop browser → shell view → background folder →
// Application). Because this object lives in explorer.exe (medium integrity),
// any launch performed through it — ShellExecute, FolderItem::InvokeVerb — runs
// at the user's integrity level rather than inheriting winzoo's admin token.
// Caller owns the returned pointer (Release() when done); returns nullptr when
// the Explorer automation object is unavailable.
IShellDispatch2* GetExplorerShellDispatch()
{
    IShellWindows* psw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER,
                                IID_PPV_ARGS(&psw))))
        return nullptr;

    IShellDispatch2* result = nullptr;
    VARIANT vEmpty; VariantInit(&vEmpty);
    VARIANT vLoc;   vLoc.vt = VT_I4; vLoc.lVal = CSIDL_DESKTOP;
    long lhwnd = 0;
    IDispatch* pdisp = nullptr;
    if (psw->FindWindowSW(&vLoc, &vEmpty, SWC_DESKTOP, &lhwnd,
                          SWFO_NEEDDISPATCH, &pdisp) == S_OK && pdisp) {
        IServiceProvider* psp = nullptr;
        if (SUCCEEDED(pdisp->QueryInterface(IID_PPV_ARGS(&psp)))) {
            IShellBrowser* psb = nullptr;
            if (SUCCEEDED(psp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&psb)))) {
                IShellView* psv = nullptr;
                if (SUCCEEDED(psb->QueryActiveShellView(&psv))) {
                    IDispatch* pdispBg = nullptr;
                    if (SUCCEEDED(psv->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&pdispBg)))) {
                        IShellFolderViewDual* psfvd = nullptr;
                        if (SUCCEEDED(pdispBg->QueryInterface(IID_PPV_ARGS(&psfvd)))) {
                            IDispatch* pdispShell = nullptr;
                            if (SUCCEEDED(psfvd->get_Application(&pdispShell))) {
                                pdispShell->QueryInterface(IID_PPV_ARGS(&result));
                                pdispShell->Release();
                            }
                            psfvd->Release();
                        }
                        pdispBg->Release();
                    }
                    psv->Release();
                }
                psb->Release();
            }
            psp->Release();
        }
        pdisp->Release();
    }
    psw->Release();
    return result;
}

// Launch through Explorer's IShellDispatch2 so the child runs at the user's
// (medium) integrity level. Returns false if the Explorer automation object is
// unavailable (caller should then fall back to a direct ShellExecute).
static bool ShellExecuteViaExplorer(const wchar_t* file, const wchar_t* params,
                                    const wchar_t* dir, const wchar_t* verb, int nShow)
{
    IShellDispatch2* psd = GetExplorerShellDispatch();
    if (!psd) return false;

    bool ok = false;
    VARIANT vArgs, vDir, vVerb, vShow;
    VariantInit(&vArgs); VariantInit(&vDir);
    VariantInit(&vVerb); VariantInit(&vShow);
    if (params && *params) { vArgs.vt = VT_BSTR; vArgs.bstrVal = SysAllocString(params); }
    if (dir && *dir)       { vDir.vt  = VT_BSTR; vDir.bstrVal  = SysAllocString(dir); }
    if (verb && *verb)     { vVerb.vt = VT_BSTR; vVerb.bstrVal = SysAllocString(verb); }
    vShow.vt = VT_I4; vShow.lVal = nShow;
    BSTR bFile = SysAllocString(file);
    if (bFile && SUCCEEDED(psd->ShellExecute(bFile, vArgs, vDir, vVerb, vShow)))
        ok = true;
    if (bFile) SysFreeString(bFile);
    VariantClear(&vArgs); VariantClear(&vDir); VariantClear(&vVerb);
    psd->Release();
    return ok;
}

bool ShellExecuteUser(HWND hwnd, const wchar_t* verb, const wchar_t* file,
                      const wchar_t* params, const wchar_t* dir, int nShow)
{
    if (IsProcessElevated() &&
        ShellExecuteViaExplorer(file, params, dir, verb, nShow))
        return true;

    // Not elevated, or the Explorer route was unavailable — launch directly.
    HINSTANCE rc = ShellExecuteW(hwnd, verb, file,
                                 (params && *params) ? params : nullptr,
                                 (dir && *dir) ? dir : nullptr, nShow);
    return reinterpret_cast<INT_PTR>(rc) > 32;
}

void ShowRunDialog(HWND hwnd)
{
    // Prefer Explorer's IShellDispatch::FileRun. The rundll32 shell32.dll,#61
    // route hosts the dialog inside rundll32 (titled "Run DLL") and, when winzoo
    // is elevated, launches its targets elevated too. FileRun runs in Explorer's
    // medium-integrity process, so we get the genuine "Run" dialog and anything
    // started from it de-elevates.
    IShellDispatch2* psd = GetExplorerShellDispatch();
    if (psd) {
        HRESULT hr = psd->FileRun();
        psd->Release();
        if (SUCCEEDED(hr)) return;
    }

    // Explorer automation object unavailable — fall back to rundll32 (the dialog
    // inherits winzoo's token in this case).
    ShellExecuteW(hwnd, L"open", L"rundll32.exe", L"shell32.dll,#61",
                  nullptr, SW_SHOWNORMAL);
}

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

    // Last resort: use the foreground window only if it actually belongs to the
    // launched exe (or its ApplicationFrameHost host). Without this check we could
    // yank an unrelated window the user switched to during the poll.
    if (!best) {
        HWND fg = GetForegroundWindow();
        if (ForegroundIsTarget(fg, exeName)) best = fg;
    }

    if (!best) return 0;

    MoveWindowToMonitor(best, hMon);  // no-op when hMon is null
    ForceForegroundWindow(best);
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

    HANDLE hProc = nullptr;
    DWORD  pid   = 0;
    if (IsProcessElevated()) {
        // Launch de-elevated via Explorer; no process handle is returned, so the
        // tracking thread relies on snapshot diff + exe-name matching instead.
        if (!ShellExecuteViaExplorer(exe, args, nullptr, L"open", nShow)) {
            // Explorer route unavailable — fall back to a direct (elevated) launch.
            HINSTANCE rc = ShellExecuteW(nullptr, L"open", exe,
                                         (args && *args) ? args : nullptr, nullptr, nShow);
            if (reinterpret_cast<INT_PTR>(rc) <= 32) return;
        }
    } else {
        SHELLEXECUTEINFOW sei = {};
        sei.cbSize       = sizeof(sei);
        sei.fMask        = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb       = L"open";
        sei.lpFile       = exe;
        sei.lpParameters = (args && *args) ? args : nullptr;
        sei.nShow        = nShow;
        if (!ShellExecuteExW(&sei)) return;
        hProc = sei.hProcess;
        pid   = hProc ? GetProcessId(hProc) : 0;
    }

    auto ctxOwner = std::make_unique<LaunchMoveCtx>();
    ctxOwner->hProc = hProc;
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

// Bring a freshly-launched window to the foreground. When winzoo is elevated it
// launches apps through Explorer's automation object so they de-elevate, which
// means the new window is created by explorer.exe — a process that didn't
// receive the click — so Windows' foreground-steal lock intermittently leaves
// the window open but unfocused. SwitchToThisWindow brings it forward across
// UAC integrity levels and bypasses that lock, mirroring how the taskbar
// activates existing app windows on click.
static void ForceForegroundWindow(HWND hwnd)
{
    if (hwnd && IsWindow(hwnd)) SwitchToThisWindow(hwnd, TRUE);
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

// True if window h's owning process has image filename `exeName` (case-insensitive).
static bool WindowProcessNameIs(HWND h, const wchar_t* exeName)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!pid) return false;

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return false;

    wchar_t path[MAX_PATH] = {};
    DWORD sz = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameW(hProc, 0, path, &sz);
    CloseHandle(hProc);
    if (!ok) return false;

    const wchar_t* fname = wcsrchr(path, L'\\');
    fname = fname ? fname + 1 : path;
    return _wcsicmp(fname, exeName) == 0;
}

// A foreground window is only an acceptable "last resort" target if it actually
// belongs to the launched exe, or to ApplicationFrameHost (which hosts UWP/WinUI
// apps under its own process name). This prevents yanking an unrelated window the
// user switched to while we were polling.
static bool ForegroundIsTarget(HWND fg, const wchar_t* exeName)
{
    if (!fg || !IsRealAppWindow(fg)) return false;
    if (exeName && exeName[0] && WindowProcessNameIs(fg, exeName)) return true;
    if (WindowProcessNameIs(fg, L"ApplicationFrameHost.exe")) return true;
    return false;
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
        if (WindowProcessNameIs(h, c.name)) {
            c.found = h;
            return FALSE;  // stop enumeration
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
    // Skip our taskbar (hwndCaller) and hwndBefore — if focus stays on the window
    // that was already foreground, the launch didn't open a new window and we must
    // not move it. A genuinely-already-foreground singleton is handled in Phase 2.
    for (int i = 0; i < 300; ++i) {
        Sleep(10);
        HWND fg = GetForegroundWindow();
        if (!fg) continue;
        if (fg == ctx->hwndCaller) continue;
        if (fg == ctx->hwndBefore) continue;
        if (!IsRealAppWindow(fg)) continue;
        // Foreground changed to a real app window — this is our target.
        MoveWindowToMonitor(fg, ctx->hMon);
        ForceForegroundWindow(fg);
        return 0;
    }

    // Phase 2: Foreground didn't change. The app was likely already foreground
    // (singleton case). Find it by process name and move it.
    HWND target = FindWindowByProcessName(ctx->exeName);
    if (target) {
        MoveWindowToMonitor(target, ctx->hMon);
        ForceForegroundWindow(target);
        return 0;
    }

    // Phase 3: Process-name scan failed (e.g. WinUI app hosted in
    // ApplicationFrameHost). Last resort: move the foreground window only if it
    // belongs to the launched exe or its ApplicationFrameHost host — never an
    // unrelated window the user may have switched to.
    HWND fg = GetForegroundWindow();
    if (fg && fg != ctx->hwndCaller && fg != ctx->hwndBefore &&
        ForegroundIsTarget(fg, ctx->exeName)) {
        MoveWindowToMonitor(fg, ctx->hMon);
        ForceForegroundWindow(fg);
    }

    return 0;
}

void LaunchOrActivateOnMonitor(HWND hwndCaller, HMONITOR hMon,
                               const wchar_t* exe, const wchar_t* args, int nShow)
{
    if (!hMon) return;

    HWND fgBefore = GetForegroundWindow();

    // Launch de-elevated when we're elevated; ShellExecuteUser reports success so
    // we don't spawn the tracking thread (which would relocate some unrelated
    // window) if the launch failed.
    if (!ShellExecuteUser(hwndCaller, L"open", exe, args, nullptr, nShow)) return;

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
