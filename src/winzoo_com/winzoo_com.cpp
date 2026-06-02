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
#include <new>

static HMODULE g_hModule  = nullptr;
static LONG    g_lockCount = 0;

// ---------------------------------------------------------------------------
// Explorer hook — poll copy dialog's progress bar on Shell_TrayWnd thread
// ---------------------------------------------------------------------------

#include <commctrl.h>  // PBM_GETPOS, PBM_GETRANGE

#pragma data_seg(".winzoo_shared")
static HHOOK  g_hook = nullptr;
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

    g_hook = SetWindowsHookExW(WH_GETMESSAGE, ExplorerGetMsgProc, g_hModule, tid);
}

extern "C" __declspec(dllexport) void __stdcall WinzooCom_UninstallHook() {
    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }
    g_explorerPid = 0;
}
