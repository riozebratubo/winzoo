#include "TrayIconProvider.h"
#include "TrayNotifyProvider.h"
#include <commctrl.h>
#include <psapi.h>
#include <shellapi.h>
#include <objbase.h>

// Layout of a notification-area button's dwData field in Explorer's process (x64).
// This is an undocumented but well-known structure used by Explorer.
// We read the first 16 bytes (hWnd + uID + uCallbackMsg) which are stable, then
// probe for the HICON at multiple offsets since the layout varies between Windows builds.
#pragma pack(push, 1)
struct TrayDataBase {
    HWND hWnd;           // 8 bytes on x64
    UINT uID;            // 4 bytes
    UINT uCallbackMsg;   // 4 bytes
};
#pragma pack(pop)

// Size of the raw TRAYDATA blob we read for icon probing.
// Windows 11 21H2+ grew the internal ICONDATA structure; the HICON can sit at
// offset 88–120. 256 bytes covers all known layouts without significant cost.
static constexpr SIZE_T kTrayDataReadSize = 256;

// Helper: read a block from another process.
static bool ReadRemote(HANDLE hProc, LPCVOID remote, void* local, SIZE_T size)
{
    SIZE_T read = 0;
    return ReadProcessMemory(hProc, remote, local, size, &read) && read == size;
}

// Helper: capture the toolbar via PrintWindow and return the pixel data as a 32-bit
// top-down ARGB DIB.  Returns nullptr on failure; caller must free with delete[].
// *outWidth and *outHeight receive the captured dimensions.
static BYTE* CaptureToolbarBitmap(HWND hToolbar, int* outWidth, int* outHeight)
{
    RECT rc = {};
    GetClientRect(hToolbar, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return nullptr;

    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem    = CreateCompatibleDC(hdcScreen);

    BITMAPINFOHEADER bih = {};
    bih.biSize        = sizeof(bih);
    bih.biWidth       = w;
    bih.biHeight      = -h; // top-down
    bih.biPlanes      = 1;
    bih.biBitCount    = 32;
    bih.biCompression = BI_RGB;

    void* pBits = nullptr;
    HBITMAP hDib = CreateDIBSection(hdcScreen, reinterpret_cast<BITMAPINFO*>(&bih),
                                    DIB_RGB_COLORS, &pBits, nullptr, 0);
    ReleaseDC(nullptr, hdcScreen);
    if (!hDib || !pBits) {
        DeleteDC(hdcMem);
        return nullptr;
    }

    HGDIOBJ hOld = SelectObject(hdcMem, hDib);

    // Clear to transparent black so we can detect drawn pixels.
    memset(pBits, 0, static_cast<size_t>(w) * h * 4);

    // PW_CLIENTONLY = 1, PW_RENDERFULLCONTENT = 2
    PrintWindow(hToolbar, hdcMem, PW_CLIENTONLY | 0x2 /*PW_RENDERFULLCONTENT*/);

    SelectObject(hdcMem, hOld);
    DeleteDC(hdcMem);

    // Copy pixel data out before deleting the DIB.
    size_t dataSize = static_cast<size_t>(w) * h * 4;
    BYTE* copy = new (std::nothrow) BYTE[dataSize];
    if (copy) memcpy(copy, pBits, dataSize);

    DeleteObject(hDib);
    *outWidth  = w;
    *outHeight = h;
    return copy;
}

// Helper: extract an HICON from a captured 32-bit BGRA bitmap at the given rect.
// Returns nullptr if the region is entirely transparent/empty.
static HICON ExtractIconFromCapture(const BYTE* pixels, int bmpW, int bmpH,
                                    const RECT& iconRect, int iconCx, int iconCy)
{
    if (!pixels || iconCx <= 0 || iconCy <= 0) return nullptr;
    if (iconRect.left < 0 || iconRect.top < 0 ||
        iconRect.right > bmpW || iconRect.bottom > bmpH)
        return nullptr;

    int srcW = iconRect.right  - iconRect.left;
    int srcH = iconRect.bottom - iconRect.top;
    if (srcW <= 0 || srcH <= 0) return nullptr;

    // We'll extract the center iconCx×iconCy area from the button rect.
    int offX = (srcW - iconCx) / 2;
    int offY = (srcH - iconCy) / 2;
    if (offX < 0) offX = 0;
    if (offY < 0) offY = 0;
    int cropW = (std::min)(iconCx, srcW);
    int cropH = (std::min)(iconCy, srcH);

    // Check if the region has any non-transparent pixels.
    bool hasContent = false;
    for (int y = 0; y < cropH && !hasContent; ++y) {
        int srcY = iconRect.top + offY + y;
        for (int x = 0; x < cropW; ++x) {
            int srcX = iconRect.left + offX + x;
            const BYTE* px = pixels + (static_cast<size_t>(srcY) * bmpW + srcX) * 4;
            // px[3] is alpha in BGRA; also check if any color channel is non-zero
            if (px[3] != 0 || px[0] != 0 || px[1] != 0 || px[2] != 0) {
                hasContent = true;
                break;
            }
        }
    }
    if (!hasContent) return nullptr;

    // Build a 32-bit BGRA DIB for the icon's color bitmap.
    BITMAPINFOHEADER bih = {};
    bih.biSize        = sizeof(bih);
    bih.biWidth       = cropW;
    bih.biHeight      = -cropH; // top-down
    bih.biPlanes      = 1;
    bih.biBitCount    = 32;
    bih.biCompression = BI_RGB;

    HDC hdcScreen = GetDC(nullptr);
    void* pColor  = nullptr;
    HBITMAP hbmColor = CreateDIBSection(hdcScreen, reinterpret_cast<BITMAPINFO*>(&bih),
                                        DIB_RGB_COLORS, &pColor, nullptr, 0);
    if (!hbmColor || !pColor) {
        ReleaseDC(nullptr, hdcScreen);
        return nullptr;
    }

    // Copy pixel data into the color bitmap.
    for (int y = 0; y < cropH; ++y) {
        int srcY = iconRect.top + offY + y;
        BYTE* dst = static_cast<BYTE*>(pColor) + static_cast<size_t>(y) * cropW * 4;
        for (int x = 0; x < cropW; ++x) {
            int srcX = iconRect.left + offX + x;
            const BYTE* src = pixels + (static_cast<size_t>(srcY) * bmpW + srcX) * 4;
            dst[x * 4 + 0] = src[0]; // B
            dst[x * 4 + 1] = src[1]; // G
            dst[x * 4 + 2] = src[2]; // R
            dst[x * 4 + 3] = src[3]; // A
        }
    }

    // Create a monochrome mask (all zeros = fully opaque; alpha is in color bitmap).
    HBITMAP hbmMask = CreateBitmap(cropW, cropH, 1, 1, nullptr);

    ICONINFO ii  = {};
    ii.fIcon     = TRUE;
    ii.hbmColor  = hbmColor;
    ii.hbmMask   = hbmMask;
    HICON hIcon  = CreateIconIndirect(&ii);

    DeleteObject(hbmColor);
    DeleteObject(hbmMask);
    ReleaseDC(nullptr, hdcScreen);
    return hIcon;
}

// Helper: get tooltip text for button index from the toolbar's tooltip control.
static std::wstring GetButtonTooltip(HANDLE hProc, HWND hToolbar, int idx, LPVOID pShared)
{
    DWORD_PTR ttResult = 0;
    if (!SendMessageTimeoutW(hToolbar, TB_GETTOOLTIPS, 0, 0,
                             SMTO_ABORTIFHUNG, 500, &ttResult) || !ttResult)
        return {};
    HWND hTT = reinterpret_cast<HWND>(ttResult);

    // Write a TOOLINFO into shared memory and request the text.
    struct RemoteTI {
        UINT      cbSize;
        UINT      uFlags;
        HWND      hwnd;
        UINT_PTR  uId;
        RECT      rect;
        HINSTANCE hinst;
        LPWSTR    lpszText; // will be pointed at the text buffer inside pShared
        LPARAM    lParam;
        void*     lpReserved;
    };

    // We place TOOLINFO at pShared, and the text buffer right after it.
    const SIZE_T kTextOffset = sizeof(RemoteTI);
    const SIZE_T kTextBufLen = 256; // wchar_t count

    RemoteTI ti    = {};
    ti.cbSize      = sizeof(RemoteTI);
    ti.uFlags      = TTF_IDISHWND;
    ti.hwnd        = hToolbar;
    ti.uId         = static_cast<UINT_PTR>(idx);
    // Point lpszText at the memory just after the TOOLINFO struct in the remote allocation.
    ti.lpszText    = reinterpret_cast<LPWSTR>(
                         static_cast<char*>(pShared) + kTextOffset);

    BYTE* pTIRemote = static_cast<BYTE*>(pShared);
    if (!WriteProcessMemory(hProc, pTIRemote, &ti, sizeof(ti), nullptr))
        return {};

    // Zero out the text area.
    std::vector<wchar_t> zeroBuf(kTextBufLen, L'\0');
    WriteProcessMemory(hProc, pTIRemote + kTextOffset, zeroBuf.data(),
                       kTextBufLen * sizeof(wchar_t), nullptr);

    SendMessageTimeoutW(hTT, TTM_GETTEXT, kTextBufLen,
                        reinterpret_cast<LPARAM>(pTIRemote),
                        SMTO_ABORTIFHUNG, 500, nullptr);

    // Read back the text.
    std::vector<wchar_t> buf(kTextBufLen, L'\0');
    SIZE_T read = 0;
    ReadProcessMemory(hProc, pTIRemote + kTextOffset, buf.data(),
                      kTextBufLen * sizeof(wchar_t), &read);
    buf.back() = L'\0';
    return std::wstring(buf.data());
}

// Helper: enumerate all icons from a single toolbar window and append to `result`.
// `hToolbar` must be a valid ToolbarWindow32 HWND.
// `hParentForCapture` is the top-level window to show/hide if needed for PrintWindow.
// `skipHidden`: if true, skip buttons with TBSTATE_HIDDEN.
static void EnumerateToolbarButtons(HWND hToolbar, HWND hParentForCapture,
                                    int iconSizePx, bool fallbackExeIcon,
                                    bool skipHidden,
                                    std::vector<TrayIconEntry>& result)
{
    DWORD_PTR btnCountResult = 0;
    if (!SendMessageTimeoutW(hToolbar, TB_BUTTONCOUNT, 0, 0,
                             SMTO_ABORTIFHUNG, 500, &btnCountResult))
        return;
    int nButtons = static_cast<int>(btnCountResult);
    if (nButtons <= 0) return;

    // Get the imagelist from the toolbar (try slots 0, 1, 2).
    HIMAGELIST hIml = nullptr;
    for (int slot = 0; slot <= 2 && !hIml; ++slot) {
        DWORD_PTR imlResult = 0;
        if (SendMessageTimeoutW(hToolbar, TB_GETIMAGELIST, slot, 0,
                                SMTO_ABORTIFHUNG, 500, &imlResult))
            hIml = reinterpret_cast<HIMAGELIST>(imlResult);
    }

    // Open Explorer's process for VM operations.
    DWORD explorerPid = 0;
    GetWindowThreadProcessId(hToolbar, &explorerPid);
    if (!explorerPid) return;

    HANDLE hProc = OpenProcess(
        PROCESS_VM_READ | PROCESS_VM_OPERATION | PROCESS_VM_WRITE, FALSE, explorerPid);
    if (!hProc) return;

    // Allocate a shared block in Explorer's address space large enough for one
    // TBBUTTON and a TOOLINFO + text buffer.
    const SIZE_T kSharedSize = sizeof(TBBUTTON) + kTrayDataReadSize + 4096;
    LPVOID pShared = VirtualAllocEx(hProc, nullptr, kSharedSize,
                                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pShared) {
        CloseHandle(hProc);
        return;
    }

    // Capture the toolbar via PrintWindow to extract icons from its rendering.
    int capW = 0, capH = 0;
    BYTE* capPixels = nullptr;
    {
        bool wasHidden = hParentForCapture && !IsWindowVisible(hParentForCapture);
        if (wasHidden) {
            SetWindowLongPtrW(hParentForCapture, GWL_EXSTYLE,
                GetWindowLongPtrW(hParentForCapture, GWL_EXSTYLE) | WS_EX_LAYERED);
            SetLayeredWindowAttributes(hParentForCapture, 0, 0, LWA_ALPHA);
            ShowWindow(hParentForCapture, SW_SHOWNOACTIVATE);
            Sleep(50);
        }

        capPixels = CaptureToolbarBitmap(hToolbar, &capW, &capH);

        if (wasHidden) {
            ShowWindow(hParentForCapture, SW_HIDE);
            SetWindowLongPtrW(hParentForCapture, GWL_EXSTYLE,
                GetWindowLongPtrW(hParentForCapture, GWL_EXSTYLE) & ~WS_EX_LAYERED);
        }
    }

    for (int i = 0; i < nButtons; ++i) {
        // Ask Explorer to write the TBBUTTON into shared memory.
        SendMessageTimeoutW(hToolbar, TB_GETBUTTON, static_cast<WPARAM>(i),
                            reinterpret_cast<LPARAM>(pShared),
                            SMTO_ABORTIFHUNG, 500, nullptr);

        TBBUTTON btn = {};
        if (!ReadRemote(hProc, pShared, &btn, sizeof(btn))) continue;

        // Skip hidden buttons if requested.
        if (skipHidden && (btn.fsState & TBSTATE_HIDDEN)) continue;

        TrayIconEntry entry = {};
        entry.uID           = static_cast<UINT>(btn.idCommand);

        BYTE trayBlob[kTrayDataReadSize] = {};
        bool hasTrayData = false;
        if (btn.dwData) {
            hasTrayData = ReadRemote(hProc, reinterpret_cast<LPCVOID>(btn.dwData),
                                    trayBlob, kTrayDataReadSize);
            if (hasTrayData) {
                auto* base = reinterpret_cast<TrayDataBase*>(trayBlob);
                entry.hWnd         = base->hWnd;
                entry.uCallbackMsg = base->uCallbackMsg;
                entry.uID          = base->uID;
            }
        }

        // Exe path of the icon's owner process.
        std::wstring exeFullPath;
        if (entry.hWnd) {
            DWORD ownerPid = 0;
            GetWindowThreadProcessId(entry.hWnd, &ownerPid);
            if (ownerPid) {
                HANDLE hOwner = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                            FALSE, ownerPid);
                if (hOwner) {
                    wchar_t path[MAX_PATH] = {};
                    DWORD sz = MAX_PATH;
                    if (QueryFullProcessImageNameW(hOwner, 0, path, &sz))
                        exeFullPath = path;
                    CloseHandle(hOwner);
                }
            }
        }
        if (!exeFullPath.empty()) {
            entry.exePath = exeFullPath;
            auto slash = exeFullPath.rfind(L'\\');
            entry.exeName = (slash != std::wstring::npos)
                            ? exeFullPath.substr(slash + 1) : exeFullPath;
        }

        // --- Icon retrieval ---

        // 1. Probe the TRAYDATA blob for a valid HICON at multiple 8-byte-aligned offsets.
        if (hasTrayData) {
            for (SIZE_T off = 16; off + sizeof(HICON) <= kTrayDataReadSize; off += 8) {
                HICON candidate = *reinterpret_cast<HICON*>(trayBlob + off);
                if (!candidate) continue;
                HICON copied = CopyIcon(candidate);
                if (copied) {
                    entry.hIcon = copied;
                    break;
                }
            }
        }

        // 2. Extract from PrintWindow capture of the toolbar.
        if (!entry.hIcon && capPixels) {
            DWORD_PTR rectResult = 0;
            SendMessageTimeoutW(hToolbar, TB_GETITEMRECT, static_cast<WPARAM>(i),
                                reinterpret_cast<LPARAM>(pShared),
                                SMTO_ABORTIFHUNG, 500, &rectResult);
            RECT btnRect = {};
            if (ReadRemote(hProc, pShared, &btnRect, sizeof(btnRect))) {
                int iconCx = iconSizePx > 0 ? iconSizePx : 16;
                int iconCy = iconCx;
                entry.hIcon = ExtractIconFromCapture(capPixels, capW, capH,
                                                    btnRect, iconCx, iconCy);
            }
        }

        // 3. Imagelist (may work if Explorer shares the imagelist handle).
        if (!entry.hIcon && hIml && btn.iBitmap >= 0)
            entry.hIcon = ImageList_GetIcon(hIml, btn.iBitmap, ILD_TRANSPARENT);

        // 4. Extract icon from the owner's exe file (optional setting).
        if (!entry.hIcon && fallbackExeIcon && !exeFullPath.empty()) {
            HICON hSmall = nullptr;
            if (ExtractIconExW(exeFullPath.c_str(), 0, nullptr, &hSmall, 1) && hSmall)
                entry.hIcon = hSmall;
        }

        // Tooltip.
        entry.tooltip = GetButtonTooltip(hProc, hToolbar, i, pShared);

        entry.orderKey = entry.exeName + L"|" + std::to_wstring(entry.uID);

        result.push_back(std::move(entry));
    }

    delete[] capPixels;
    VirtualFreeEx(hProc, pShared, 0, MEM_RELEASE);
    CloseHandle(hProc);
}

// ── ITrayNotify COM-based enumeration ────────────────────────────────────────

// INotificationCB implementation: Explorer calls Notify() once per registered
// Shell_NotifyIcon icon when we call ITrayNotify::RegisterCallback.
class TrayNotifyCB final : public INotificationCB
{
    std::vector<TrayIconEntry>& out_;
    LONG refs_ = 1;
public:
    explicit TrayNotifyCB(std::vector<TrayIconEntry>& out) : out_(out) {}

    HRESULT STDMETHODCALLTYPE Notify(ULONG_PTR, NOTIFYITEM* item) override {
        if (!item) return S_OK;
        // Do NOT filter on IsWindow(hWnd): system icons (Bluetooth, Eject
        // Hardware, etc.) can arrive with a null or destroyed hWnd and are
        // still valid tray registrations.
        TrayIconEntry e = {};
        e.hWnd  = item->hWnd;
        e.uID   = item->uID;
        // uCallbackMsg is not in NOTIFYITEM; filled in by EnrichCallbackMsgs().
        e.hIcon = item->hIcon ? CopyIcon(item->hIcon) : nullptr;
        if (item->pszTip)     e.tooltip = item->pszTip;
        if (item->pszExeName) e.exeName = item->pszExeName;
        // Derive exePath for exe-icon fallback.
        if (item->hWnd) {
            DWORD pid = 0;
            GetWindowThreadProcessId(item->hWnd, &pid);
            if (pid) {
                HANDLE hOwner = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                            FALSE, pid);
                if (hOwner) {
                    wchar_t path[MAX_PATH] = {};
                    DWORD sz = MAX_PATH;
                    if (QueryFullProcessImageNameW(hOwner, 0, path, &sz))
                        e.exePath = path;
                    CloseHandle(hOwner);
                }
            }
        }
        e.orderKey = e.exeName + L"|" + std::to_wstring(e.uID);
        out_.push_back(std::move(e));
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&refs_));
    }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&refs_);
        if (!r) delete this;
        return static_cast<ULONG>(r);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == __uuidof(INotificationCB))
            { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
};

// Try to enumerate tray icons via ITrayNotify.
// Returns an empty vector on any failure; caller falls back to toolbar approach.
static std::vector<TrayIconEntry> EnumerateTrayIconsViaCOM()
{
    ITrayNotify* pTN = nullptr;
    if (FAILED(CoCreateInstance(CLSID_TrayNotify, nullptr,
                                CLSCTX_LOCAL_SERVER,
                                __uuidof(ITrayNotify),
                                reinterpret_cast<void**>(&pTN))))
        return {};

    std::vector<TrayIconEntry> result;
    auto* cb = new TrayNotifyCB(result);
    ULONG_PTR handle = 0;
    HRESULT hr = pTN->RegisterCallback(cb, &handle);
    if (SUCCEEDED(hr)) {
        // RegisterCallback delivers all current icons synchronously via COM's
        // STA re-entrant dispatch before returning. No Refresh() needed, and
        // calling it can fault on Windows 11 builds with a different vtable.
        pTN->UnregisterCallback(&handle);
    }
    cb->Release();
    pTN->Release();

    if (FAILED(hr) || result.empty()) return {};
    return result;
}

// Locate Explorer's main tray toolbar (Windows 10/11 compatible).
static HWND FindTrayToolbar()
{
    HWND hTray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!hTray) return nullptr;
    HWND hNotify = FindWindowExW(hTray, nullptr, L"TrayNotifyWnd", nullptr);
    if (!hNotify) return nullptr;
    HWND hPager = FindWindowExW(hNotify, nullptr, L"SysPager", nullptr);
    if (hPager) {
        HWND h = FindWindowExW(hPager, nullptr, L"ToolbarWindow32", nullptr);
        if (h) return h;
    }
    return FindWindowExW(hNotify, nullptr, L"ToolbarWindow32", nullptr);
}

// Fill in uCallbackMsg for each entry by reading only the 16-byte TrayDataBase
// from Explorer's toolbar buttons. No icon extraction is performed.
static void EnrichCallbackMsgs(HWND hToolbar,
                                std::vector<TrayIconEntry>& icons)
{
    if (!hToolbar || icons.empty()) return;

    DWORD_PTR btnCountResult = 0;
    if (!SendMessageTimeoutW(hToolbar, TB_BUTTONCOUNT, 0, 0,
                             SMTO_ABORTIFHUNG, 500, &btnCountResult))
        return;
    int nButtons = static_cast<int>(btnCountResult);
    if (nButtons <= 0) return;

    DWORD explorerPid = 0;
    GetWindowThreadProcessId(hToolbar, &explorerPid);
    if (!explorerPid) return;

    HANDLE hProc = OpenProcess(
        PROCESS_VM_READ | PROCESS_VM_OPERATION | PROCESS_VM_WRITE,
        FALSE, explorerPid);
    if (!hProc) return;

    const SIZE_T kBufSize = sizeof(TBBUTTON) + sizeof(TrayDataBase) + 64;
    LPVOID pShared = VirtualAllocEx(hProc, nullptr, kBufSize,
                                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pShared) { CloseHandle(hProc); return; }

    for (int i = 0; i < nButtons; ++i) {
        SendMessageTimeoutW(hToolbar, TB_GETBUTTON, static_cast<WPARAM>(i),
                            reinterpret_cast<LPARAM>(pShared),
                            SMTO_ABORTIFHUNG, 500, nullptr);
        TBBUTTON btn = {};
        if (!ReadRemote(hProc, pShared, &btn, sizeof(btn))) continue;
        if (!btn.dwData) continue;

        TrayDataBase base = {};
        if (!ReadRemote(hProc, reinterpret_cast<LPCVOID>(btn.dwData),
                        &base, sizeof(base)))
            continue;
        if (!base.hWnd || !base.uCallbackMsg) continue;

        for (auto& e : icons) {
            if (e.hWnd == base.hWnd && e.uID == base.uID) {
                e.uCallbackMsg = base.uCallbackMsg;
                break;
            }
        }
    }

    VirtualFreeEx(hProc, pShared, 0, MEM_RELEASE);
    CloseHandle(hProc);
}

// ─────────────────────────────────────────────────────────────────────────────

// Build the full toolbar-based icon list (both main toolbar and overflow).
static std::vector<TrayIconEntry> EnumerateViaToolbar(int iconSizePx,
                                                      bool fallbackExeIcon,
                                                      bool includeOverflow)
{
    HWND hTray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!hTray) return {};
    HWND hNotify = FindWindowExW(hTray, nullptr, L"TrayNotifyWnd", nullptr);
    if (!hNotify) return {};

    HWND hToolbar = nullptr;
    {
        HWND hPager = FindWindowExW(hNotify, nullptr, L"SysPager", nullptr);
        if (hPager)
            hToolbar = FindWindowExW(hPager, nullptr, L"ToolbarWindow32", nullptr);
    }
    if (!hToolbar)
        hToolbar = FindWindowExW(hNotify, nullptr, L"ToolbarWindow32", nullptr);
    if (!hToolbar) return {};

    std::vector<TrayIconEntry> result;
    EnumerateToolbarButtons(hToolbar, hTray, iconSizePx, fallbackExeIcon,
                            /*skipHidden=*/!includeOverflow, result);

    if (includeOverflow) {
        HWND hOverflow = FindWindowW(L"NotifyIconOverflowWindow", nullptr);
        if (hOverflow) {
            HWND hOverflowToolbar = FindWindowExW(hOverflow, nullptr,
                                                  L"ToolbarWindow32", nullptr);
            if (hOverflowToolbar)
                EnumerateToolbarButtons(hOverflowToolbar, hOverflow,
                                        iconSizePx, fallbackExeIcon,
                                        /*skipHidden=*/true, result);
        }
    }
    return result;
}

std::vector<TrayIconEntry> EnumerateTrayIcons(int iconSizePx, bool fallbackExeIcon,
                                              bool includeOverflow)
{
    // Lead with ITrayNotify (COM): delivers hIcon directly from Explorer with no
    // TRAYDATA probing, and now covers all registrations (IsWindow guard removed,
    // Refresh() added) including system icons like Bluetooth and Eject Hardware.
    auto result = EnumerateTrayIconsViaCOM();

    // Always run the toolbar too — it provides uCallbackMsg (absent from
    // NOTIFYITEM) and acts as a safety net for any icon ITrayNotify misses.
    auto tbResult = EnumerateViaToolbar(iconSizePx, fallbackExeIcon, includeOverflow);

    if (result.empty()) return tbResult;  // COM unavailable — use toolbar only.

    // Fill in uCallbackMsg for COM entries via the lightweight TRAYDATA base read.
    HWND hToolbar = FindTrayToolbar();
    if (hToolbar) EnrichCallbackMsgs(hToolbar, result);

    // Supplement: add toolbar entries whose hWnd is known (TRAYDATA readable) but
    // whose hWnd+uID pair isn't already in the COM result.  Skip null-hWnd toolbar
    // ghosts — they have no reliable identity and would only add blank slots.
    for (auto& te : tbResult) {
        if (!te.hWnd) continue;
        bool already = false;
        for (const auto& ce : result) {
            if (ce.hWnd == te.hWnd && ce.uID == te.uID) { already = true; break; }
        }
        if (!already) result.push_back(std::move(te));
    }

    return result;
}
