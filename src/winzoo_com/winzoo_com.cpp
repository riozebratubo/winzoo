// winzoo_com.dll — COM InProcServer32 wrapper for CLSID_TaskbarList.
//
// Registered by Winzoo under HKCU\Software\Classes\CLSID\{56FDF344...}\InProcServer32.
// When any app calls CoCreateInstance(CLSID_TaskbarList), COM loads this DLL and
// creates a CTaskbarListProxy. SetProgressValue and SetProgressState are intercepted
// and relayed to Winzoo via PostMessage. All other methods return S_OK stubs —
// no shell32 forwarding needed since Winzoo hides Explorer's taskbar.
//
// Additionally exports WinzooCom_InstallHook / WinzooCom_UninstallHook to intercept
// Explorer's internal progress messages (0x04F3) on its Shell_TrayWnd thread.

#include <windows.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <winternl.h>
#include <new>
#include <vector>
#include "../WinzooTrayIpc.h"

static HMODULE g_hModule  = nullptr;
static LONG    g_lockCount = 0;

// ---------------------------------------------------------------------------
// Explorer hook — poll copy dialog's progress bar on Shell_TrayWnd thread
// ---------------------------------------------------------------------------

#include <commctrl.h>  // PBM_GETPOS, PBM_GETRANGE

#pragma data_seg(".winzoo_shared")
static HHOOK  g_hook = nullptr;     // WH_GETMESSAGE — progress polling
static HHOOK  g_cwpHook = nullptr;  // WH_CALLWNDPROC — tray Shell_NotifyIcon intercept
static DWORD  g_explorerPid = 0;
#pragma data_seg()
#pragma comment(linker, "/SECTION:.winzoo_shared,RWS")

// Cached copy dialog HWND — avoids repeated EnumWindows
static HWND  g_cachedCopyDialog = nullptr;
static DWORD g_lastPollTick = 0;
static constexpr DWORD kPollIntervalMs = 200;

static HWND FindExplorerCopyDialog() {
    // If we have a cached target, verify it's still valid
    if (g_cachedCopyDialog) {
        if (IsWindow(g_cachedCopyDialog) && IsWindowVisible(g_cachedCopyDialog)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(g_cachedCopyDialog, &pid);
            if (pid == g_explorerPid)
                return g_cachedCopyDialog;
        }
        g_cachedCopyDialog = nullptr;
    }

    // Find a visible, captioned, top-level Explorer-process window that is NOT
    // a folder (CabinetWClass) or shell infrastructure.
    struct Ctx {
        DWORD pid;
        HWND  result;
    } ctx{ g_explorerPid, nullptr };

    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        if (!IsWindowVisible(hwnd)) return TRUE;

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != c->pid) return TRUE;

        LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (exStyle & WS_EX_TOOLWINDOW) return TRUE;

        LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        if (!(style & WS_CAPTION)) return TRUE;

        if (GetWindowTextLengthW(hwnd) == 0) return TRUE;

        wchar_t cls[128] = {};
        GetClassNameW(hwnd, cls, 128);

        // Skip known shell/folder/system classes
        if (wcscmp(cls, L"CabinetWClass") == 0) return TRUE;
        if (wcscmp(cls, L"Shell_TrayWnd") == 0) return TRUE;
        if (wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0) return TRUE;
        if (wcscmp(cls, L"Progman") == 0) return TRUE;
        if (wcscmp(cls, L"WorkerW") == 0) return TRUE;
        if (wcscmp(cls, L"NotifyIconOverflowWindow") == 0) return TRUE;
        if (wcscmp(cls, L"TopLevelWindowForOverflowXamlIsland") == 0) return TRUE;
        if (wcscmp(cls, L"XamlExplorerHostIslandWindow") == 0) return TRUE;

        c->result = hwnd;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&ctx));

    g_cachedCopyDialog = ctx.result;
    return ctx.result;
}

static HWND FindProgressBarChild(HWND parent) {
    // Find a progress bar control (msctls_progress32) among children/descendants
    struct Ctx {
        HWND result;
    } ctx{ nullptr };

    EnumChildWindows(parent, [](HWND hwnd, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        wchar_t cls[64] = {};
        GetClassNameW(hwnd, cls, 64);
        if (wcscmp(cls, L"msctls_progress32") == 0) {
            c->result = hwnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    return ctx.result;
}

static void PollCopyDialogProgress() {
    HWND dialog = FindExplorerCopyDialog();
    if (!dialog) return;

    HWND progressBar = FindProgressBarChild(dialog);
    if (!progressBar) return;

    // Read the progress bar's current position and range
    PBRANGE range = {};
    SendMessageW(progressBar, PBM_GETRANGE, FALSE, reinterpret_cast<LPARAM>(&range));
    int pos = static_cast<int>(SendMessageW(progressBar, PBM_GETPOS, 0, 0));

    int total = range.iHigh - range.iLow;
    if (total <= 0) return;

    int pct = (pos - range.iLow) * 100 / total;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    // Relay to Winzoo
    UINT msg = RegisterWindowMessageW(L"WinzooProgress");
    if (!msg) return;

    HWND winzoo = FindWindowW(L"WinzooTaskbar", nullptr);
    while (winzoo) {
        PostMessageW(winzoo, msg, reinterpret_cast<WPARAM>(dialog),
                     MAKELPARAM(2 /*TBPF_NORMAL*/, pct));
        winzoo = FindWindowExW(nullptr, winzoo, L"WinzooTaskbar", nullptr);
    }
}

static LRESULT CALLBACK ExplorerGetMsgProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0) {
        // Poll the copy dialog's progress bar every kPollIntervalMs
        DWORD now = GetTickCount();
        if (now - g_lastPollTick >= kPollIntervalMs) {
            g_lastPollTick = now;
            PollCopyDialogProgress();
        }
    }
    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Tray interception — observe Shell_NotifyIcon on Explorer's Shell_TrayWnd
// thread via WH_CALLWNDPROC and relay each add/modify/delete to Winzoo.
//
// When an app calls Shell_NotifyIcon, shell32 SendMessage's WM_COPYDATA to
// Shell_TrayWnd with COPYDATASTRUCT.dwData == 1 and lpData -> SHELLTRAYDATA.
// The WH_CALLWNDPROC hook runs in Explorer's process (where it is injected),
// so the marshaled COPYDATASTRUCT and the NOTIFYICONDATA.hIcon are valid here.
// ---------------------------------------------------------------------------

// Marshaled tray-data blob carried by the WM_COPYDATA(dwData==1) message.
// CRITICAL: the wire NOTIFYICONDATA is NOT the in-process NOTIFYICONDATAW. It is a
// PACKED structure with 32-bit handle fields (hWnd/hIcon/hBalloonIcon are DWORDs, not
// pointers) — the format predates x64 and window/icon handles fit in 32 bits anyway.
// Verified by dumping the raw blob: dwMagic=0x34753423, cbSize=956, fields at the
// 4-byte-packed offsets below.
#pragma pack(push, 1)
struct WireNID {
    DWORD cbSize;            // +0   (== 956)
    DWORD hWnd;              // +4   32-bit window handle
    DWORD uID;              // +8
    DWORD uFlags;           // +12
    DWORD uCallbackMessage; // +16
    DWORD hIcon;            // +20  32-bit icon handle
    WCHAR szTip[128];       // +24
    DWORD dwState;          // +280
    DWORD dwStateMask;      // +284
    WCHAR szInfo[256];      // +288
    DWORD uVersion;         // +800 (union with uTimeout)
    WCHAR szInfoTitle[64];  // +804
    DWORD dwInfoFlags;      // +932
    GUID  guidItem;         // +936
    DWORD hBalloonIcon;     // +952 32-bit
};
struct SHELLTRAYDATA {
    DWORD   dwMagic;        // +0  version cookie 0x34753423
    DWORD   dwMessage;      // +4  NIM_ADD / NIM_MODIFY / NIM_DELETE / NIM_SETVERSION
    WireNID nid;            // +8
};
#pragma pack(pop)

// Convert an HICON to top-down BGRA bits. Handles 32bpp alpha icons and falls
// back to deriving alpha from the mono mask for legacy icons.
static bool IconToBGRA(HICON hIcon, std::vector<BYTE>& out, int& w, int& h) {
    out.clear(); w = h = 0;
    if (!hIcon) return false;

    ICONINFO ii = {};
    if (!GetIconInfo(hIcon, &ii)) return false;

    bool ok = false;
    HDC  hdc = GetDC(nullptr);
    BITMAP bm = {};
    if (hdc && ii.hbmColor && GetObjectW(ii.hbmColor, sizeof(bm), &bm) &&
        bm.bmWidth > 0 && bm.bmHeight > 0 && bm.bmWidth <= 256 && bm.bmHeight <= 256) {
        w = bm.bmWidth;
        h = bm.bmHeight;

        BITMAPINFO bi = {};
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = w;
        bi.bmiHeader.biHeight      = -h;   // top-down
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        out.resize(static_cast<size_t>(w) * h * 4);
        if (GetDIBits(hdc, ii.hbmColor, 0, h, out.data(), &bi, DIB_RGB_COLORS)) {
            bool hasAlpha = false;
            for (size_t i = 3; i < out.size(); i += 4)
                if (out[i] != 0) { hasAlpha = true; break; }

            if (!hasAlpha) {
                // Derive alpha from the mask: read 1bpp mask as 32bpp (0 = opaque).
                std::vector<BYTE> mask(static_cast<size_t>(w) * h * 4);
                if (ii.hbmMask &&
                    GetDIBits(hdc, ii.hbmMask, 0, h, mask.data(), &bi, DIB_RGB_COLORS)) {
                    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
                        out[i * 4 + 3] = mask[i * 4] ? 0 : 255;
                } else {
                    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
                        out[i * 4 + 3] = 255;
                }
            }
            ok = true;
        } else {
            out.clear();
        }
    }

    if (hdc) ReleaseDC(nullptr, hdc);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask)  DeleteObject(ii.hbmMask);
    return ok;
}

// Relay one tray record to every WinzooTaskbar window via WM_COPYDATA.
static void RelayTrayRecord(const SHELLTRAYDATA* st, SIZE_T cbData) {
    // Bound: we need at least through hIcon (end of the fixed head, before szTip).
    constexpr SIZE_T kMinHead = offsetof(SHELLTRAYDATA, nid) + offsetof(WireNID, szTip);
    if (cbData < kMinHead) return;

    const WireNID& nid = st->nid;

    WinzooTrayRecord rec = {};
    rec.dwMessage    = st->dwMessage;
    rec.reserved     = sizeof(WinzooTrayRecord);  // signals header size to receiver
    rec.ownerHwnd    = static_cast<UINT64>(nid.hWnd);   // 32-bit handle, zero-extended
    rec.uID          = nid.uID;
    rec.uFlags       = nid.uFlags;
    rec.uCallbackMsg = (nid.uFlags & NIF_MESSAGE) ? nid.uCallbackMessage : 0;

    // guidItem and uVersion live deep in the struct — only read when the full blob
    // is present (best-effort; both are optional).
    if (cbData >= sizeof(SHELLTRAYDATA)) {
        if (nid.uFlags & NIF_GUID) rec.guidItem = nid.guidItem;
        rec.uVersion = nid.uVersion;  // meaningful for NIM_SETVERSION
    }

    // State (NIS_HIDDEN etc.) — dwState/dwStateMask are at fixed offsets past szTip.
    // Only safe to read when the blob covers through dwStateMask.
    constexpr SIZE_T kMinState = offsetof(SHELLTRAYDATA, nid) +
                                 offsetof(WireNID, dwStateMask) + sizeof(DWORD);
    if ((nid.uFlags & kNIF_STATE) && cbData >= kMinState) {
        rec.dwState     = nid.dwState;
        rec.dwStateMask = nid.dwStateMask;
    }

    // Tooltip (szTip) — bounded by both the field size AND how much of the blob is
    // actually present, then NUL-clamped. A truncated record could otherwise make us
    // walk szTip past the end of the marshaled buffer (caught by the caller's
    // __except, but a garbage tooltip is still better avoided).
    std::vector<wchar_t> tip;
    constexpr SIZE_T kTipStart = offsetof(SHELLTRAYDATA, nid) + offsetof(WireNID, szTip);
    if ((nid.uFlags & NIF_TIP) && cbData > kTipStart) {
        const wchar_t* p = nid.szTip;
        size_t maxChars = sizeof(nid.szTip) / sizeof(wchar_t);
        size_t avail    = (cbData - kTipStart) / sizeof(wchar_t);
        if (avail < maxChars) maxChars = avail;
        size_t n = 0;
        while (n < maxChars && p[n]) ++n;
        tip.assign(p, p + n);
    }

    // Icon -> BGRA. Only on add/modify with NIF_ICON. hIcon is a 32-bit handle.
    std::vector<BYTE> bgra;
    int iw = 0, ih = 0;
    if ((st->dwMessage == NIM_ADD || st->dwMessage == NIM_MODIFY) &&
        (nid.uFlags & NIF_ICON) && nid.hIcon) {
        IconToBGRA(reinterpret_cast<HICON>(static_cast<UINT_PTR>(nid.hIcon)), bgra, iw, ih);
    }

    rec.iconW      = iw;
    rec.iconH      = ih;
    rec.cbIconBits = static_cast<UINT>(bgra.size());
    rec.cbTooltip  = static_cast<UINT>(tip.size() * sizeof(wchar_t));

    // Assemble [record][tooltip][bgra] into one contiguous buffer.
    std::vector<BYTE> buf(sizeof(rec) + rec.cbTooltip + rec.cbIconBits);
    memcpy(buf.data(), &rec, sizeof(rec));
    if (rec.cbTooltip)
        memcpy(buf.data() + sizeof(rec), tip.data(), rec.cbTooltip);
    if (rec.cbIconBits)
        memcpy(buf.data() + sizeof(rec) + rec.cbTooltip, bgra.data(), rec.cbIconBits);

    COPYDATASTRUCT cds = {};
    cds.dwData = kWinzooTrayMagic;
    cds.cbData = static_cast<DWORD>(buf.size());
    cds.lpData = buf.data();

    HWND winzoo = FindWindowW(L"WinzooTaskbar", nullptr);
    while (winzoo) {
        DWORD_PTR res = 0;
        // SMTO_BLOCK is REQUIRED for WM_COPYDATA: the receiver reads cds.lpData
        // (our buf) during its synchronous handling, and blocking keeps this relay
        // from being reentered mid-marshal by the next intercepted Shell_NotifyIcon
        // during the TaskbarCreated re-registration storm — which is exactly when a
        // burst of icons arrives. This mirrors how shell32 itself delivers tray
        // registrations. (An earlier build dropped SMTO_BLOCK to break a suspected
        // winzoo<->Explorer deadlock; a live dump later showed the real "hang" was
        // proxy Z-order occlusion, not a deadlock, and dropping the flag lost tray
        // icons. winzoo also no longer makes unbounded synchronous calls into this
        // thread — taskbars hide async, the ITrayNotify probe is bounded — so there
        // is nothing here to deadlock against.) SMTO_ABORTIFHUNG + 300ms bounds a
        // genuinely wedged winzoo.
        SendMessageTimeoutW(winzoo, WM_COPYDATA, 0,
                            reinterpret_cast<LPARAM>(&cds),
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 300, &res);
        winzoo = FindWindowExW(nullptr, winzoo, L"WinzooTaskbar", nullptr);
    }
}

// ---------------------------------------------------------------------------
// Work-area veto — IAT-hook SHAppBarMessage inside Explorer and collapse Explorer's
// OWN taskbar appbar reservations to zero thickness, so they stop stacking under
// winzoo's appbar (the "appbar fight" / 2.5x reserved-space stacking). winzoo hides
// those taskbars anyway. Identified by window class so third-party appbars are
// untouched. Discovered via spike: Explorer reserves via ABM_QUERYPOS/ABM_SETPOS in a
// reassertion loop, then applies the combined work area via SPI_SETWORKAREA.
// ---------------------------------------------------------------------------

typedef UINT_PTR (WINAPI *SHAppBarMessage_t)(DWORD, PAPPBARDATA);

static SHAppBarMessage_t g_realSHAppBarMessage = nullptr;
static bool              g_spikeInstalled      = false;

// Is this one of Explorer's own taskbar windows (whose reservation we suppress)?
static bool IsExplorerTaskbar(HWND h) {
    if (!h) return false;
    wchar_t cls[32] = {};
    if (!GetClassNameW(h, cls, 32)) return false;
    return wcscmp(cls, L"Shell_TrayWnd") == 0 ||
           wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0;
}

// Collapse a rect to zero thickness on the given appbar edge.
static void CollapseEdge(RECT& rc, UINT edge) {
    switch (edge) {
        case ABE_BOTTOM: rc.top    = rc.bottom; break;
        case ABE_TOP:    rc.bottom = rc.top;    break;
        case ABE_LEFT:   rc.right  = rc.left;   break;
        case ABE_RIGHT:  rc.left   = rc.right;  break;
        default: break;
    }
}

// Infer the docked edge (ABE_*) of a window from its rect relative to its monitor.
static UINT EdgeFromWindowRect(HWND h) {
    RECT r{};
    if (!GetWindowRect(h, &r)) return ABE_BOTTOM;
    HMONITOR hm = MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfo(hm, &mi)) return ABE_BOTTOM;
    RECT m = mi.rcMonitor;
    int w = r.right - r.left, ht = r.bottom - r.top;
    if (w >= ht)  // horizontal bar -> top or bottom
        return (r.top - m.top) <= (m.bottom - r.bottom) ? ABE_TOP : ABE_BOTTOM;
    return (r.left - m.left) <= (m.right - r.right) ? ABE_LEFT : ABE_RIGHT;  // vertical
}

// Find winzoo's taskbar on the same monitor as `ref`, and return its docked edge.
// Returns false if no winzoo bar is on that monitor (then we leave the edge alone).
static bool WinzooEdgeForMonitor(HWND ref, UINT& outEdge) {
    HMONITOR refMon = MonitorFromWindow(ref, MONITOR_DEFAULTTOPRIMARY);
    for (HWND h = FindWindowW(L"WinzooTaskbar", nullptr); h;
         h = FindWindowExW(nullptr, h, L"WinzooTaskbar", nullptr)) {
        if (MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY) == refMon) {
            outEdge = EdgeFromWindowRect(h);
            return true;
        }
    }
    return false;
}

static UINT_PTR WINAPI Hook_SHAppBarMessage(DWORD dwMessage, PAPPBARDATA abd) {
    // For Explorer's OWN taskbars on ABM_SETPOS, rewrite the registration so it (1)
    // docks on winzoo's edge and (2) reserves zero thickness:
    //   - (c) work-area: collapsing the rect to zero stops Explorer's taskbar strip from
    //     stacking its reservation under winzoo's (the "appbar fight"). Primary fix.
    //   - (b) minimize: genuine ApplicationFrameWindow/UWP apps (e.g. Settings) resolve
    //     the taskbar from the *registered appbar edge*, not ptMinPosition (verified by
    //     test). Keeping Explorer's appbar on winzoo's edge keeps those minimizing toward
    //     winzoo even mid-session. (Win11's Task Manager is a XAML-island special case
    //     that ignores this and still needs the BeforeExplorer restart path.)
    if (abd && dwMessage == ABM_SETPOS && IsExplorerTaskbar(abd->hWnd)) {
        UINT edge = abd->uEdge;
        UINT wedge;
        if (WinzooEdgeForMonitor(abd->hWnd, wedge))
            edge = wedge;
        abd->uEdge = edge;
        CollapseEdge(abd->rc, edge);
    }
    return g_realSHAppBarMessage(dwMessage, abd);
}

// Replace every IAT slot in `hMod` whose bound address == target with `repl`.
static void PatchModuleIAT(HMODULE hMod, void* target, void* repl) {
    auto* base = reinterpret_cast<BYTE*>(hMod);
    auto* dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    DWORD impRVA = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!impRVA) return;

    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + impRVA);
         imp->Name; ++imp) {
        if (!imp->FirstThunk) continue;
        for (auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
             thunk->u1.Function; ++thunk) {
            void** slot = reinterpret_cast<void**>(&thunk->u1.Function);
            if (*slot != target) continue;
            DWORD oldProt = 0;
            if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProt)) {
                *slot = repl;
                VirtualProtect(slot, sizeof(void*), oldProt, &oldProt);
            }
        }
    }
}

// Enumerate loaded modules via the PEB loader list. Unlike Toolhelp this is pure
// memory reads, so it is safe to call from DllMain (under the loader lock) when we
// must unpatch before the DLL unmaps — otherwise the IAT slots would dangle and the
// next SHAppBarMessage call in Explorer would jump into freed memory.
static void PatchAllModules(void* target, void* repl) {
#ifdef _WIN64
    auto* peb = reinterpret_cast<PPEB>(__readgsqword(0x60));
#else
    auto* peb = reinterpret_cast<PPEB>(__readfsdword(0x30));
#endif
    if (!peb || !peb->Ldr) return;
    LIST_ENTRY* head = &peb->Ldr->InMemoryOrderModuleList;
    for (LIST_ENTRY* e = head->Flink; e && e != head; e = e->Flink) {
        auto* ent = CONTAINING_RECORD(e, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
        if (ent->DllBase)
            PatchModuleIAT(reinterpret_cast<HMODULE>(ent->DllBase), target, repl);
    }
}

static void InstallWorkAreaSpike() {
    if (g_spikeInstalled) return;
    g_spikeInstalled = true;

    g_realSHAppBarMessage = reinterpret_cast<SHAppBarMessage_t>(
        GetProcAddress(GetModuleHandleW(L"shell32.dll"), "SHAppBarMessage"));

    if (g_realSHAppBarMessage) {
        __try {
            PatchAllModules(reinterpret_cast<void*>(g_realSHAppBarMessage),
                            reinterpret_cast<void*>(Hook_SHAppBarMessage));
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

// Restore the original SHAppBarMessage IAT slots before the DLL unmaps from Explorer.
static void UninstallWorkAreaSpike() {
    if (!g_spikeInstalled || !g_realSHAppBarMessage) return;
    __try {
        PatchAllModules(reinterpret_cast<void*>(Hook_SHAppBarMessage),
                        reinterpret_cast<void*>(g_realSHAppBarMessage));
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_spikeInstalled = false;
}

static LRESULT CALLBACK ExplorerCallWndProc(int nCode, WPARAM wParam, LPARAM lParam) {
    InstallWorkAreaSpike();  // one-time, runs inside Explorer on first dispatch
    if (nCode == HC_ACTION) {
        auto* cwp = reinterpret_cast<CWPSTRUCT*>(lParam);
        if (cwp && cwp->message == WM_COPYDATA) {
            auto* cds = reinterpret_cast<COPYDATASTRUCT*>(cwp->lParam);
            if (cds && cds->dwData == 1 && cds->lpData && cds->cbData >= sizeof(DWORD) * 2) {
                wchar_t cls[32] = {};
                GetClassNameW(cwp->hwnd, cls, 32);
                if (wcscmp(cls, L"Shell_TrayWnd") == 0) {
                    __try {
                        RelayTrayRecord(reinterpret_cast<SHELLTRAYDATA*>(cds->lpData),
                                        cds->cbData);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        // Never destabilize Explorer over a malformed tray blob.
                    }
                }
            }
        }
    }
    return CallNextHookEx(g_cwpHook, nCode, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Relay helpers — find Winzoo's window and post progress data to it
// ---------------------------------------------------------------------------

static UINT GetRelayMsg() {
    static UINT s_msg = 0;
    if (!s_msg) s_msg = RegisterWindowMessageW(L"WinzooProgress");
    return s_msg;
}

static void RelayProgress(HWND appHwnd, int state, int pct) {
    UINT msg = GetRelayMsg();
    if (!msg) return;
    HWND winzoo = FindWindowW(L"WinzooTaskbar", nullptr);
    while (winzoo) {
        PostMessageW(winzoo, msg, reinterpret_cast<WPARAM>(appHwnd), MAKELPARAM(state, pct));
        winzoo = FindWindowExW(nullptr, winzoo, L"WinzooTaskbar", nullptr);
    }
}

// ---------------------------------------------------------------------------
// CTaskbarListProxy — intercepts progress, stubs everything else
// ---------------------------------------------------------------------------

class CTaskbarListProxy final : public ITaskbarList4 {
public:
    CTaskbarListProxy() : m_ref(1) { InterlockedIncrement(&g_lockCount); }
    ~CTaskbarListProxy()           { InterlockedDecrement(&g_lockCount); }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        static const IID IID_ITaskbarList4_local =
            {0xC43DC798,0x95D1,0x4BEA,{0x90,0x30,0xBB,0x99,0xE2,0x98,0x3A,0x1A}};
        if (riid == IID_IUnknown        || riid == IID_ITaskbarList  ||
            riid == IID_ITaskbarList2   || riid == IID_ITaskbarList3 ||
            riid == IID_ITaskbarList4_local) {
            *ppv = static_cast<ITaskbarList4*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&m_ref));
    }
    STDMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }

    // ITaskbarList — stubs (Explorer taskbar is hidden by Winzoo)
    STDMETHODIMP HrInit()                        override { return S_OK; }
    STDMETHODIMP AddTab(HWND)                    override { return S_OK; }
    STDMETHODIMP DeleteTab(HWND)                 override { return S_OK; }
    STDMETHODIMP ActivateTab(HWND)               override { return S_OK; }
    STDMETHODIMP SetActiveAlt(HWND)              override { return S_OK; }

    // ITaskbarList2
    STDMETHODIMP MarkFullscreenWindow(HWND, BOOL) override { return S_OK; }

    // ITaskbarList3 — intercept progress, stub everything else
    STDMETHODIMP SetProgressValue(HWND hwnd, ULONGLONG completed,
                                  ULONGLONG total) override {
        if (total > 0) {
            int pct = static_cast<int>(completed * 100 / total);
            if (pct < 0) pct = 0; else if (pct > 100) pct = 100;
            RelayProgress(hwnd, 2 /*TBPF_NORMAL*/, pct);
        }
        return S_OK;
    }
    STDMETHODIMP SetProgressState(HWND hwnd, TBPFLAG flags) override {
        RelayProgress(hwnd, static_cast<int>(flags), 0);
        return S_OK;
    }
    STDMETHODIMP RegisterTab(HWND, HWND)              override { return S_OK; }
    STDMETHODIMP UnregisterTab(HWND)                  override { return S_OK; }
    STDMETHODIMP SetTabOrder(HWND, HWND)              override { return S_OK; }
    STDMETHODIMP SetTabActive(HWND, HWND, DWORD)      override { return S_OK; }
    STDMETHODIMP ThumbBarAddButtons(HWND, UINT, LPTHUMBBUTTON)    override { return S_OK; }
    STDMETHODIMP ThumbBarUpdateButtons(HWND, UINT, LPTHUMBBUTTON) override { return S_OK; }
    STDMETHODIMP ThumbBarSetImageList(HWND, HIMAGELIST)           override { return S_OK; }
    STDMETHODIMP SetOverlayIcon(HWND, HICON, LPCWSTR)             override { return S_OK; }
    STDMETHODIMP SetThumbnailTooltip(HWND, LPCWSTR)               override { return S_OK; }
    STDMETHODIMP SetThumbnailClip(HWND, RECT*)                    override { return S_OK; }

    // ITaskbarList4
    STDMETHODIMP SetTabProperties(HWND, STPFLAG) override { return S_OK; }

private:
    LONG m_ref;
};

// ---------------------------------------------------------------------------
// CTaskbarListFactory — statically allocated, not ref-counted
// ---------------------------------------------------------------------------

class CTaskbarListFactory final : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IClassFactory)
            { *ppv = this; return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef()  override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }

    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* p = new (std::nothrow) CTaskbarListProxy();
        if (!p) return E_OUTOFMEMORY;
        HRESULT hr = p->QueryInterface(riid, ppv);
        p->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) InterlockedIncrement(&g_lockCount);
        else      InterlockedDecrement(&g_lockCount);
        return S_OK;
    }
};

static CTaskbarListFactory g_factory;

// ---------------------------------------------------------------------------
// DLL entry point — COM exports live in winzoo_com_exports.cpp
// ---------------------------------------------------------------------------

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hMod;
        DisableThreadLibraryCalls(hMod);
    } else if (reason == DLL_PROCESS_DETACH) {
        // In Explorer, restore the SHAppBarMessage IAT before we unmap so its slots
        // don't dangle. No-op in winzoo's own process (spike never installed there).
        UninstallWorkAreaSpike();
    }
    return TRUE;
}

HRESULT __stdcall WinzooCom_GetClassObject(const GUID& rclsid, const GUID& riid, void** ppv) {
    static const CLSID CLSID_TaskbarList_local =
        {0x56FDF344,0xFD6D,0x11d0,{0x95,0x8A,0x00,0x60,0x97,0xC9,0xA0,0x90}};
    if (rclsid != CLSID_TaskbarList_local) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}

HRESULT __stdcall WinzooCom_CanUnload() {
    return g_lockCount == 0 ? S_OK : S_FALSE;
}

// ---------------------------------------------------------------------------
// Explorer hook exports — called by Winzoo (TaskbarProxy) to intercept
// Explorer's internal progress messages on its Shell_TrayWnd thread.
// ---------------------------------------------------------------------------

extern "C" __declspec(dllexport) void __stdcall WinzooCom_InstallHook(HWND explorerTrayWnd) {
    if (g_hook) return;  // already installed

    DWORD tid = GetWindowThreadProcessId(explorerTrayWnd, &g_explorerPid);
    if (!tid) return;

    // WH_GETMESSAGE drives copy-dialog progress polling; WH_CALLWNDPROC observes
    // the Shell_NotifyIcon WM_COPYDATA traffic delivered to Shell_TrayWnd.
    g_hook    = SetWindowsHookExW(WH_GETMESSAGE,  ExplorerGetMsgProc, g_hModule, tid);
    g_cwpHook = SetWindowsHookExW(WH_CALLWNDPROC, ExplorerCallWndProc, g_hModule, tid);
}

extern "C" __declspec(dllexport) void __stdcall WinzooCom_UninstallHook() {
    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }
    if (g_cwpHook) {
        UnhookWindowsHookEx(g_cwpHook);
        g_cwpHook = nullptr;
    }
    g_explorerPid = 0;
}
