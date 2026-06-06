#include "SettingsDialog.h"
#include <algorithm>
#include <utility>
#include <commctrl.h>
#include <commdlg.h>
#include <uxtheme.h>
#include "Registry.h"
#include "resource.h"

static constexpr const wchar_t* kPositions[] = {
    L"Top", L"Bottom", L"Left", L"Right", L"Floating"
};

static constexpr const wchar_t* kAppMenuLayouts[] = {
    L"Simple List", L"Simple Grid", L"Classic", L"Classic Rounded"
};

static constexpr const wchar_t* kMinimizedIndicatorTypes[] = {
    L"Small rectangle on the bottom", L"Dim the button"
};

static constexpr const wchar_t* kTaskbarMonitorModes[] = {
    L"On all monitors", L"On primary monitor"
};

// Order must match enum TaskbarHookMethod { None, BeforeExplorer }.
// A third method ("After Explorer", value 2) is reserved but unimplemented and so is
// intentionally omitted here — see the comment on TaskbarHookMethod in Settings.h.
static constexpr const wchar_t* kTaskbarHookMethods[] = {
    L"None",
    L"Before Explorer (restarts explorer)"
};

struct DlgData {
    Settings* settings;
    HWND      hScrollHosts[6] = {};
    int       selectedTheme   = 0;
    // Layout metrics (dialog client coordinates, set in WM_INITDIALOG, used by WM_SIZE)
    bool layoutReady    = false;
    int  tabL           = 0;  // tab control left
    int  tabT           = 0;  // tab control top
    int  tabRightGap    = 0;  // client right  - tab right
    int  tabBotGap      = 0;  // client bottom - tab bottom
    int  btnH           = 0;  // button height
    int  btnBotGap      = 0;  // client bottom - button bottom
    int  btnDefL        = 0;  // Defaults button left
    int  btnDefW        = 0;  // Defaults button width
    int  btnOKRightGap  = 0;  // client right  - OK right edge
    int  btnOKW         = 0;  // OK button width
    int  btnCxlRightGap = 0;  // client right  - Cancel right edge
    int  btnCxlW        = 0;  // Cancel button width
    int  minW           = 0;  // minimum tracking window width  (px)
    int  minH           = 0;  // minimum tracking window height (px)
};

// Scrollable panel used for every settings tab — scrolls its children vertically
static constexpr wchar_t kScrollHostClass[] = L"WinzooTabScrollHost";

// Reposition every child by -delta px (no per-child repaints), then
// synchronously erase + repaint everything in one shot to avoid the
// transparent-control garbling that ScrollWindowEx causes.
static void ScrollHostBy(HWND hwnd, int delta)
{
    HDWP hdwp = BeginDeferWindowPos(32);
    for (HWND hc = GetWindow(hwnd, GW_CHILD); hc; hc = GetWindow(hc, GW_HWNDNEXT)) {
        RECT r;
        GetWindowRect(hc, &r);
        MapWindowPoints(HWND_DESKTOP, hwnd, reinterpret_cast<LPPOINT>(&r), 2);
        if (hdwp)
            hdwp = DeferWindowPos(hdwp, hc, nullptr, r.left, r.top - delta, 0, 0,
                                  SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
    }
    if (hdwp) EndDeferWindowPos(hdwp);
    RedrawWindow(hwnd, nullptr, nullptr,
                 RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

static int ScrollHostApplyPos(HWND hwnd, SCROLLINFO& si, int newPos)
{
    int maxPos = si.nMax - static_cast<int>(si.nPage) + 1;
    newPos = std::max(0, std::min(newPos, maxPos));
    if (newPos == si.nPos) return 0;
    int delta = newPos - si.nPos;
    si.nPos  = newPos;
    si.fMask = SIF_POS;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
    ScrollHostBy(hwnd, delta);
    return delta;
}

static LRESULT CALLBACK AppMenuScrollHostProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(reinterpret_cast<HDC>(wParam), &rc,
                 reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1));
        return 1;
    }
    case WM_PAINT: {
        // Background is handled entirely in WM_ERASEBKGND.
        // Just validate the region so the system stops sending WM_PAINT.
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_VSCROLL: {
        SCROLLINFO si = {};
        si.cbSize = sizeof(si);
        si.fMask  = SIF_ALL;
        GetScrollInfo(hwnd, SB_VERT, &si);
        int newPos = si.nPos;
        switch (LOWORD(wParam)) {
        case SB_LINEUP:        newPos -= 15; break;
        case SB_LINEDOWN:      newPos += 15; break;
        case SB_PAGEUP:        newPos -= static_cast<int>(si.nPage); break;
        case SB_PAGEDOWN:      newPos += static_cast<int>(si.nPage); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
            SCROLLINFO si2 = {};
            si2.cbSize = sizeof(si2);
            si2.fMask  = SIF_TRACKPOS;
            GetScrollInfo(hwnd, SB_VERT, &si2);
            newPos = si2.nTrackPos;
            break;
        }
        default: break;
        }
        ScrollHostApplyPos(hwnd, si, newPos);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        SCROLLINFO si = {};
        si.cbSize = sizeof(si);
        si.fMask  = SIF_ALL;
        GetScrollInfo(hwnd, SB_VERT, &si);
        int lines  = (GET_WHEEL_DELTA_WPARAM(wParam) > 0) ? -3 : 3;
        ScrollHostApplyPos(hwnd, si, si.nPos + lines * 15);
        return 0;
    }
    case WM_COMMAND:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_DRAWITEM:
    case WM_NOTIFY:
        return SendMessageW(GetParent(hwnd), uMsg, wParam, lParam);
    default: break;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

static void RegisterScrollHostClass()
{
    WNDCLASSEXW existing = {};
    existing.cbSize = sizeof(existing);
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    if (GetClassInfoExW(hInst, kScrollHostClass, &existing)) return;
    WNDCLASSEXW wc    = {};
    wc.cbSize         = sizeof(wc);
    wc.lpfnWndProc    = AppMenuScrollHostProc;
    wc.hInstance      = hInst;
    wc.hCursor        = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground  = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName  = kScrollHostClass;
    RegisterClassExW(&wc);
}

// Creates a scrollable host panel, reparents the given controls into it, and
// sets up the vertical scrollbar. Returns the panel HWND (or nullptr on failure).
static HWND CreateTabScrollHost(HWND hwnd, const int* controls,
                                int left, int top, int w, int h)
{
    HWND hPanel = CreateWindowExW(
        WS_EX_CONTROLPARENT, kScrollHostClass, nullptr,
        WS_CHILD | WS_VSCROLL,
        left, top, w, h,
        hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hPanel) return nullptr;

    int contentH = 0;
    for (const int* id = controls; *id; ++id) {
        HWND hCtrl = GetDlgItem(hwnd, *id);
        if (!hCtrl) continue;
        RECT r;
        GetWindowRect(hCtrl, &r);
        SetParent(hCtrl, hPanel);
        MapWindowPoints(HWND_DESKTOP, hPanel, reinterpret_cast<LPPOINT>(&r), 2);
        SetWindowPos(hCtrl, nullptr, r.left, r.top, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        contentH = std::max(contentH, static_cast<int>(r.bottom));
    }
    contentH += 4;

    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask  = SIF_ALL;
    si.nMin   = 0;
    si.nMax   = contentH;
    si.nPage  = static_cast<UINT>(h);
    si.nPos   = 0;
    SetScrollInfo(hPanel, SB_VERT, &si, TRUE);
    return hPanel;
}

// Returns the scroll host for the given tab, or hwnd as a fallback.
static HWND TabHost(DlgData* data, int tab, HWND hwnd)
{
    if (data && data->hScrollHosts[tab]) return data->hScrollHosts[tab];
    return hwnd;
}

// Null-terminated control ID lists per tab
static const int kGeneralControls[] = {
    IDC_LBL_POSITION,  IDC_COMBO_POSITION,
    IDC_LBL_THICKNESS, IDC_EDIT_THICKNESS, IDC_SPIN_THICKNESS,
    IDC_LBL_TASKBAR_MONITOR, IDC_COMBO_TASKBAR_MONITOR,
    IDC_CHECK_APPMENU_ALL_MONITORS,
    IDC_CHECK_CURRENT_MONITOR_APPS,
    IDC_CHECK_PINNED_PER_MONITOR,
    IDC_CHECK_OPEN_SAME_MONITOR,
    IDC_CHECK_VERTICAL_TITLES,
    IDC_LBL_VERTICAL_BTN_H, IDC_EDIT_VERTICAL_BTN_H, IDC_SPIN_VERTICAL_BTN_H,
    IDC_LBL_TASKBAR_HOOK, IDC_COMBO_TASKBAR_HOOK,
    0
};
static const int kAppBtnControls[] = {
    IDC_LBL_MAXBTNW,        IDC_EDIT_MAXBTNW, IDC_SPIN_MAXBTNW,
    IDC_LBL_MINBTNW,        IDC_EDIT_MINBTNW, IDC_SPIN_MINBTNW,
    IDC_LBL_APP_BTN_ICON_SZ, IDC_EDIT_APP_BTN_ICON_SZ, IDC_SPIN_APP_BTN_ICON_SZ,
    IDC_CHECK_MIDDLECLICK,  IDC_CHECK_RIGHTCLICKGAP,
    IDC_CHECK_MINIMIZED_INDICATOR,
    IDC_LBL_MINIMIZED_INDICATOR_TYPE, IDC_COMBO_MINIMIZED_INDICATOR_TYPE,
    IDC_LBL_MINIMIZED_INDICATOR_W, IDC_EDIT_MINIMIZED_INDICATOR_W, IDC_SPIN_MINIMIZED_INDICATOR_W,
    IDC_LBL_MINIMIZED_INDICATOR_H, IDC_EDIT_MINIMIZED_INDICATOR_H, IDC_SPIN_MINIMIZED_INDICATOR_H,
    IDC_CHECK_PINNED_AS_BUTTONS,
    IDC_CHECK_PROGRESSBAR,
    IDC_CHECK_PROGRESSBAR_THEMECLR,
    IDC_LBL_PROGRESSBAR_COLOR, IDC_BTN_PROGRESSBAR_COLOR,
    IDC_LBL_PROGRESSBAR_HEIGHT, IDC_EDIT_PROGRESSBAR_HEIGHT, IDC_SPIN_PROGRESSBAR_HEIGHT,
    IDC_LBL_BTN_OUTLINE_RADIUS, IDC_EDIT_BTN_OUTLINE_RADIUS, IDC_SPIN_BTN_OUTLINE_RADIUS,
    IDC_CHECK_SHOW_PINNED_AS_BUTTONS,
    IDC_CHECK_SHOW_SEPARATORS,
    IDC_LBL_SEPARATOR_COLOR, IDC_BTN_SEPARATOR_COLOR,
    0
};
static const int kClockControls[] = {
    IDC_CHECK_SHOWCLOCK,
    IDC_LBL_TIMEFMT,  IDC_EDIT_TIMEFMT,
    IDC_LBL_DATEFMT,  IDC_EDIT_DATEFMT,
    IDC_LBL_CLOCKW,   IDC_EDIT_CLOCKW,  IDC_SPIN_CLOCKW,
    IDC_LBL_LINESPACING, IDC_EDIT_LINESPACING, IDC_SPIN_LINESPACING,
    IDC_LBL_TIMEFONTSIZE, IDC_EDIT_TIMEFONTSIZE, IDC_SPIN_TIMEFONTSIZE,
    IDC_LBL_DATEFONTSIZE, IDC_EDIT_DATEFONTSIZE, IDC_SPIN_DATEFONTSIZE,
    IDC_LBL_TIMECOLOR, IDC_BTN_TIMECOLOR,
    IDC_LBL_DATECOLOR, IDC_BTN_DATECOLOR,
    IDC_LBL_TOKENS,
    0
};

static const int kAppMenuControls[] = {
    IDC_LBL_APPMENU_LAYOUT,    IDC_COMBO_APPMENU_LAYOUT,
    IDC_LBL_APPMENU_WIDTH,     IDC_EDIT_APPMENU_WIDTH,    IDC_SPIN_APPMENU_WIDTH,
    IDC_LBL_APPMENU_MAXHEIGHT, IDC_EDIT_APPMENU_MAXHEIGHT, IDC_SPIN_APPMENU_MAXHEIGHT,
    IDC_LBL_APPMENU_ENTRYH,    IDC_EDIT_APPMENU_ENTRYH,   IDC_SPIN_APPMENU_ENTRYH,
    IDC_LBL_APPMENU_GRIDCOLS,  IDC_EDIT_APPMENU_GRIDCOLS, IDC_SPIN_APPMENU_GRIDCOLS,
    IDC_LBL_APPMENU_GRIDROWS,  IDC_EDIT_APPMENU_GRIDROWS, IDC_SPIN_APPMENU_GRIDROWS,
    IDC_LBL_APPMENU_LISTFS,    IDC_EDIT_APPMENU_LISTFS,   IDC_SPIN_APPMENU_LISTFS,
    IDC_LBL_APPMENU_GRIDFS,    IDC_EDIT_APPMENU_GRIDFS,   IDC_SPIN_APPMENU_GRIDFS,
    IDC_LBL_APPMENU_MARGIN,    IDC_EDIT_APPMENU_MARGIN,   IDC_SPIN_APPMENU_MARGIN,
    IDC_LBL_APPMENU_PADDING,   IDC_EDIT_APPMENU_PADDING,  IDC_SPIN_APPMENU_PADDING,
    IDC_CHECK_APPMENU_SIDEBAR,
    IDC_LBL_APPMENU_SIDEBARW,  IDC_EDIT_APPMENU_SIDEBARW, IDC_SPIN_APPMENU_SIDEBARW,
    IDC_CHECK_APPMENU_SIDEBAR_EXPLORER,
    IDC_CHECK_APPMENU_SIDEBAR_SETTINGS,
    IDC_CHECK_APPMENU_SIDEBAR_POWER,
    IDC_CHECK_APPMENU_FLATTEN_SUBMENUS,
    IDC_CHECK_APPMENU_FLATTEN_ALL,
    IDC_CHECK_APPMENU_SEARCH,
    IDC_CHECK_APPMENU_SEARCH_FUZZY,
    IDC_CHECK_APPMENU_SEARCH_SYSTEM,
    IDC_LBL_APPMENU_CLASSIC_PW,  IDC_EDIT_APPMENU_CLASSIC_PW, IDC_SPIN_APPMENU_CLASSIC_PW,
    IDC_CHECK_APPMENU_CLASSIC_DOCS,
    IDC_CHECK_APPMENU_CLASSIC_PICS,
    IDC_CHECK_APPMENU_CLASSIC_MUSIC,
    IDC_CHECK_APPMENU_CLASSIC_DLOADS,
    IDC_CHECK_APPMENU_CLASSIC_RECENT,
    IDC_CHECK_APPMENU_CLASSIC_THISPC,
    IDC_CHECK_APPMENU_CLASSIC_CTRL,
    IDC_CHECK_APPMENU_CLASSIC_WINSETT,
    IDC_CHECK_APPMENU_CLASSIC_RUN,
    IDC_CHECK_APPMENU_CLASSIC_SHUTDOWN,
    IDC_CHECK_APPMENU_PINNED_PER_MONITOR,
    0
};

static const int kSysTrayControls[] = {
    IDC_CHECK_TRAY_ICONS,
    IDC_CHECK_TRAY_FALLBACK_EXE,
    IDC_CHECK_TRAY_OVERFLOW,
    IDC_LBL_TRAY_ICON_SIZE,    IDC_EDIT_TRAY_ICON_SIZE,    IDC_SPIN_TRAY_ICON_SIZE,
    IDC_LBL_TRAY_ICON_PADDING, IDC_EDIT_TRAY_ICON_PADDING, IDC_SPIN_TRAY_ICON_PADDING,
    IDC_LBL_TRAY_ICON_MARGIN,  IDC_EDIT_TRAY_ICON_MARGIN,  IDC_SPIN_TRAY_ICON_MARGIN,
    IDC_CHECK_STATUS_ZONE,
    IDC_CHECK_HIDE_DEFAULT_TRAY_ICONS,
    0
};

// Visual tab controls are all created dynamically — nothing to reparent from the dialog.
static const int kVisualControls[] = { 0 };

static const int* kTabGroups[] = {
    kGeneralControls, kVisualControls, kAppBtnControls,
    kClockControls, kAppMenuControls, kSysTrayControls
};

static void ShowTab(HWND hwnd, int tab, DlgData* data)
{
    for (int g = 0; g < 6; ++g) {
        bool visible = (g == tab);
        HWND hHost = (data && data->hScrollHosts[g]) ? data->hScrollHosts[g] : nullptr;
        if (hHost) {
            ShowWindow(hHost, visible ? SW_SHOW : SW_HIDE);
        } else {
            int cmd = visible ? SW_SHOW : SW_HIDE;
            for (const int* id = kTabGroups[g]; *id; ++id)
                ShowWindow(GetDlgItem(hwnd, *id), cmd);
        }
    }
}

static void SetClockControlsEnabled(HWND hwnd, bool enabled)
{
    static const int kIds[] = {
        IDC_LBL_TIMEFMT,  IDC_EDIT_TIMEFMT,
        IDC_LBL_DATEFMT,  IDC_EDIT_DATEFMT,
        IDC_LBL_CLOCKW,   IDC_EDIT_CLOCKW,  IDC_SPIN_CLOCKW,
        IDC_LBL_LINESPACING, IDC_EDIT_LINESPACING, IDC_SPIN_LINESPACING,
        IDC_LBL_TIMEFONTSIZE, IDC_EDIT_TIMEFONTSIZE, IDC_SPIN_TIMEFONTSIZE,
        IDC_LBL_DATEFONTSIZE, IDC_EDIT_DATEFONTSIZE, IDC_SPIN_DATEFONTSIZE,
        IDC_LBL_TIMECOLOR, IDC_BTN_TIMECOLOR,
        IDC_LBL_DATECOLOR, IDC_BTN_DATECOLOR,
        IDC_LBL_TOKENS,
        0
    };
    for (const int* id = kIds; *id; ++id)
        EnableWindow(GetDlgItem(hwnd, *id), enabled ? TRUE : FALSE);
}

static void SetSidebarControlsEnabled(HWND hwnd, bool enabled)
{
    static const int kIds[] = {
        IDC_LBL_APPMENU_SIDEBARW, IDC_EDIT_APPMENU_SIDEBARW, IDC_SPIN_APPMENU_SIDEBARW,
        IDC_CHECK_APPMENU_SIDEBAR_EXPLORER,
        IDC_CHECK_APPMENU_SIDEBAR_SETTINGS,
        IDC_CHECK_APPMENU_SIDEBAR_POWER,
        0
    };
    for (const int* id = kIds; *id; ++id)
        EnableWindow(GetDlgItem(hwnd, *id), enabled ? TRUE : FALSE);
}

static void SetSearchControlsEnabled(HWND hwnd, bool enabled)
{
    EnableWindow(GetDlgItem(hwnd, IDC_CHECK_APPMENU_SEARCH_FUZZY),  enabled ? TRUE : FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_CHECK_APPMENU_SEARCH_SYSTEM), enabled ? TRUE : FALSE);
}

static void SetMinimizedIndicatorControlsEnabled(HWND hwnd, bool enabled)
{
    EnableWindow(GetDlgItem(hwnd, IDC_LBL_MINIMIZED_INDICATOR_TYPE), enabled ? TRUE : FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_COMBO_MINIMIZED_INDICATOR_TYPE), enabled ? TRUE : FALSE);

    // Size controls only apply when type is SmallRectangle
    bool sizeEnabled = enabled;
    if (enabled) {
        int sel = static_cast<int>(
            SendMessageW(GetDlgItem(hwnd, IDC_COMBO_MINIMIZED_INDICATOR_TYPE), CB_GETCURSEL, 0, 0));
        sizeEnabled = (sel == static_cast<int>(MinimizedIndicatorType::SmallRectangle));
    }
    static const int kSizeIds[] = {
        IDC_LBL_MINIMIZED_INDICATOR_W, IDC_EDIT_MINIMIZED_INDICATOR_W, IDC_SPIN_MINIMIZED_INDICATOR_W,
        IDC_LBL_MINIMIZED_INDICATOR_H, IDC_EDIT_MINIMIZED_INDICATOR_H, IDC_SPIN_MINIMIZED_INDICATOR_H,
        0
    };
    for (const int* id = kSizeIds; *id; ++id)
        EnableWindow(GetDlgItem(hwnd, *id), sizeEnabled ? TRUE : FALSE);
}

static void SetSeparatorControlsEnabled(HWND hwnd, bool enabled)
{
    EnableWindow(GetDlgItem(hwnd, IDC_LBL_SEPARATOR_COLOR), enabled ? TRUE : FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_BTN_SEPARATOR_COLOR), enabled ? TRUE : FALSE);
}

static void SetProgressBarControlsEnabled(HWND hwnd, bool pbEnabled, bool useTheme)
{
    EnableWindow(GetDlgItem(hwnd, IDC_CHECK_PROGRESSBAR_THEMECLR), pbEnabled ? TRUE : FALSE);
    bool colorEnabled = pbEnabled && !useTheme;
    EnableWindow(GetDlgItem(hwnd, IDC_LBL_PROGRESSBAR_COLOR),  colorEnabled ? TRUE : FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_BTN_PROGRESSBAR_COLOR),  colorEnabled ? TRUE : FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_LBL_PROGRESSBAR_HEIGHT), pbEnabled ? TRUE : FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_EDIT_PROGRESSBAR_HEIGHT),pbEnabled ? TRUE : FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_SPIN_PROGRESSBAR_HEIGHT),pbEnabled ? TRUE : FALSE);
}

static void SetVerticalTitleControlsEnabled(HWND hwnd, bool enabled)
{
    EnableWindow(GetDlgItem(hwnd, IDC_LBL_VERTICAL_BTN_H),  enabled ? TRUE : FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_EDIT_VERTICAL_BTN_H), enabled ? TRUE : FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_SPIN_VERTICAL_BTN_H), enabled ? TRUE : FALSE);
}

static void SetAllMonitorsControlsEnabled(HWND hwnd, bool enabled)
{
    EnableWindow(GetDlgItem(hwnd, IDC_CHECK_APPMENU_ALL_MONITORS),  enabled ? TRUE : FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_CHECK_CURRENT_MONITOR_APPS),  enabled ? TRUE : FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_CHECK_PINNED_PER_MONITOR),    enabled ? TRUE : FALSE);
}

// Creates all controls for the Visual tab inside its scroll host (hScrollHosts[1]).
// Called once from WM_INITDIALOG after the scroll host is created.
static void SetupVisualTab(HWND hwndDlg, DlgData* data, int panelW, int panelH)
{
    HWND hPanel = data->hScrollHosts[1];
    if (!hPanel) return;

    int dpi  = GetDpiForWindow(hwndDlg);
    auto px  = [dpi](int b) { return MulDiv(b, dpi, 96); };
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    HFONT hFont = reinterpret_cast<HFONT>(SendMessageW(hwndDlg, WM_GETFONT, 0, 0));

    int mgn    = px(8);
    int swW    = px(58);
    int swH    = px(72);
    int swGap  = px(6);
    int perRow = 3;
    int y      = mgn;

    // "Theme" label
    int lblH = px(13);
    HWND hLbl = CreateWindowExW(0, L"STATIC", L"Theme",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        mgn, y, panelW - 2*mgn, lblH,
        hPanel, nullptr, hInst, nullptr);
    if (hFont && hLbl) SendMessageW(hLbl, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), FALSE);
    y += lblH + px(5);

    // Swatch buttons — one per theme preset
    for (int i = 0; i < kThemePresetCount; ++i) {
        int col = i % perRow;
        int row = i / perRow;
        int x   = mgn + col * (swW + swGap);
        int sy  = y   + row * (swH + swGap);
        HWND hSw = CreateWindowExW(0, L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            x, sy, swW, swH,
            hPanel,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_THEME_SWATCH_BASE + i)),
            hInst, nullptr);
        if (hFont && hSw) SendMessageW(hSw, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), FALSE);
    }

    int nRows = (kThemePresetCount + perRow - 1) / perRow;
    y += nRows * swH + (nRows - 1) * swGap;
    y += px(14);

    // Checkbox: "Override taskbar color"
    int chkH = px(13);
    HWND hChk = CreateWindowExW(0, L"BUTTON", L"Override taskbar color",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        mgn, y, panelW - 2*mgn, chkH,
        hPanel,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CHECK_CUSTOM_TASKBAR_COLOR)),
        hInst, nullptr);
    if (hFont && hChk) SendMessageW(hChk, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), FALSE);
    SetWindowTheme(hChk, L"", L"");
    y += chkH + px(5);

    // "Taskbar color:" label + color picker button
    int rowH     = px(16);
    int lbl2W    = px(88);
    int colorBtnW = px(32);
    HWND hLbl2 = CreateWindowExW(0, L"STATIC", L"Taskbar color:",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
        mgn, y, lbl2W, rowH,
        hPanel, nullptr, hInst, nullptr);
    if (hFont && hLbl2) SendMessageW(hLbl2, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), FALSE);

    HWND hColorBtn = CreateWindowExW(0, L"BUTTON", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        mgn + lbl2W + px(4), y, colorBtnW, rowH,
        hPanel,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_CUSTOM_TASKBAR_COLOR)),
        hInst, nullptr);
    (void)hColorBtn;
    y += rowH + mgn;

    // Set initial control states
    data->selectedTheme = static_cast<int>(data->settings->theme);
    CheckDlgButton(hPanel, IDC_CHECK_CUSTOM_TASKBAR_COLOR,
                   data->settings->useCustomTaskbarColor ? BST_CHECKED : BST_UNCHECKED);
    EnableWindow(GetDlgItem(hPanel, IDC_BTN_CUSTOM_TASKBAR_COLOR),
                 data->settings->useCustomTaskbarColor ? TRUE : FALSE);

    // Set up scroll info based on content height
    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask  = SIF_ALL;
    si.nMin   = 0;
    si.nMax   = y;
    si.nPage  = static_cast<UINT>(panelH);
    si.nPos   = 0;
    SetScrollInfo(hPanel, SB_VERT, &si, TRUE);
}

// Pushes all current settings values into dialog controls.
// Safe to call multiple times (structure like combo items / spin ranges
// must already be set up by WM_INITDIALOG before this is called).
static void ApplySettingsToControls(HWND hwnd, DlgData* data)
{
    const Settings& s = *data->settings;

    HWND hGen  = TabHost(data, 0, hwnd);
    HWND hVis  = TabHost(data, 1, hwnd);
    HWND hBtn  = TabHost(data, 2, hwnd);
    HWND hClk  = TabHost(data, 3, hwnd);
    HWND hAm   = TabHost(data, 4, hwnd);
    HWND hTray = TabHost(data, 5, hwnd);

    // General tab
    SendMessageW(GetDlgItem(hGen, IDC_COMBO_POSITION), CB_SETCURSEL, static_cast<WPARAM>(s.position), 0);
    SendMessageW(GetDlgItem(hGen, IDC_SPIN_THICKNESS), UDM_SETPOS32, 0, s.thickness);
    SendMessageW(GetDlgItem(hGen, IDC_COMBO_TASKBAR_MONITOR), CB_SETCURSEL,
                 static_cast<WPARAM>(s.taskbarMonitorMode), 0);
    SendMessageW(GetDlgItem(hGen, IDC_COMBO_TASKBAR_HOOK), CB_SETCURSEL,
                 static_cast<WPARAM>(s.taskbarHookMethod), 0);
    CheckDlgButton(hGen, IDC_CHECK_APPMENU_ALL_MONITORS,
                   s.showAppMenuOnAllMonitors ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hGen, IDC_CHECK_CURRENT_MONITOR_APPS,
                   s.showCurrentMonitorAppsOnly ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hGen, IDC_CHECK_PINNED_PER_MONITOR,
                   s.pinnedAppsPerMonitor ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hGen, IDC_CHECK_OPEN_SAME_MONITOR,
                   s.openAppsOnSameMonitor ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hGen, IDC_CHECK_VERTICAL_TITLES,
                   s.showTitlesOnVertical ? BST_CHECKED : BST_UNCHECKED);
    SendMessageW(GetDlgItem(hGen, IDC_SPIN_VERTICAL_BTN_H), UDM_SETPOS32, 0, s.leftRightHeight);
    SetVerticalTitleControlsEnabled(hGen, s.showTitlesOnVertical);
    SetAllMonitorsControlsEnabled(hGen, s.taskbarMonitorMode == TaskbarMonitorMode::AllMonitors);

    // Visual tab
    if (hVis) {
        data->selectedTheme = static_cast<int>(s.theme);
        InvalidateRect(hVis, nullptr, TRUE);
        CheckDlgButton(hVis, IDC_CHECK_CUSTOM_TASKBAR_COLOR,
                       s.useCustomTaskbarColor ? BST_CHECKED : BST_UNCHECKED);
        InvalidateRect(GetDlgItem(hVis, IDC_BTN_CUSTOM_TASKBAR_COLOR), nullptr, FALSE);
        EnableWindow(GetDlgItem(hVis, IDC_BTN_CUSTOM_TASKBAR_COLOR),
                     s.useCustomTaskbarColor ? TRUE : FALSE);
    }

    // System Tray tab
    CheckDlgButton(hTray, IDC_CHECK_TRAY_ICONS,
                   s.showTrayIcons ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hTray, IDC_CHECK_TRAY_FALLBACK_EXE,
                   s.trayIconFallbackExe ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hTray, IDC_CHECK_TRAY_OVERFLOW,
                   s.showOverflowTrayIcons ? BST_CHECKED : BST_UNCHECKED);
    SendMessageW(GetDlgItem(hTray, IDC_SPIN_TRAY_ICON_SIZE),    UDM_SETPOS32, 0, s.trayIconSize);
    SendMessageW(GetDlgItem(hTray, IDC_SPIN_TRAY_ICON_PADDING), UDM_SETPOS32, 0, s.trayIconPadding);
    SendMessageW(GetDlgItem(hTray, IDC_SPIN_TRAY_ICON_MARGIN),  UDM_SETPOS32, 0, s.trayIconMargin);
    CheckDlgButton(hTray, IDC_CHECK_STATUS_ZONE,
                   s.showStatusZone ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hTray, IDC_CHECK_HIDE_DEFAULT_TRAY_ICONS,
                   s.showWinzooCustomIcons ? BST_CHECKED : BST_UNCHECKED);

    // App Buttons tab
    SendMessageW(GetDlgItem(hBtn, IDC_SPIN_MAXBTNW),        UDM_SETPOS32, 0, s.maxButtonWidth);
    SendMessageW(GetDlgItem(hBtn, IDC_SPIN_MINBTNW),        UDM_SETPOS32, 0, s.minButtonWidth);
    SendMessageW(GetDlgItem(hBtn, IDC_SPIN_APP_BTN_ICON_SZ), UDM_SETPOS32, 0, s.appButtonIconSize);
    CheckDlgButton(hBtn, IDC_CHECK_MIDDLECLICK,
                   s.middleClickClose ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hBtn, IDC_CHECK_RIGHTCLICKGAP,
                   s.showRightClickGap ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hBtn, IDC_CHECK_MINIMIZED_INDICATOR,
                   s.showMinimizedIndicator ? BST_CHECKED : BST_UNCHECKED);
    SendMessageW(GetDlgItem(hBtn, IDC_COMBO_MINIMIZED_INDICATOR_TYPE), CB_SETCURSEL,
                 static_cast<WPARAM>(s.minimizedIndicatorType), 0);
    SendMessageW(GetDlgItem(hBtn, IDC_SPIN_MINIMIZED_INDICATOR_W), UDM_SETPOS32, 0, s.minimizedIndicatorW);
    SendMessageW(GetDlgItem(hBtn, IDC_SPIN_MINIMIZED_INDICATOR_H), UDM_SETPOS32, 0, s.minimizedIndicatorH);
    SetMinimizedIndicatorControlsEnabled(hBtn, s.showMinimizedIndicator);
    CheckDlgButton(hBtn, IDC_CHECK_PINNED_AS_BUTTONS,
                   s.pinnedAppsAsButtonsWhenOpen ? BST_CHECKED : BST_UNCHECKED);

    // Progress bars
    CheckDlgButton(hBtn, IDC_CHECK_PROGRESSBAR,
                   s.showProgressBars ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hBtn, IDC_CHECK_PROGRESSBAR_THEMECLR,
                   s.progressBarUseThemeColor ? BST_CHECKED : BST_UNCHECKED);
    SendMessageW(GetDlgItem(hBtn, IDC_SPIN_PROGRESSBAR_HEIGHT), UDM_SETPOS32, 0, s.progressBarHeight);
    InvalidateRect(GetDlgItem(hBtn, IDC_BTN_PROGRESSBAR_COLOR), nullptr, FALSE);
    SetProgressBarControlsEnabled(hBtn, s.showProgressBars, s.progressBarUseThemeColor);
    SendMessageW(GetDlgItem(hBtn, IDC_SPIN_BTN_OUTLINE_RADIUS), UDM_SETPOS32, 0, s.buttonOutlineRadius);
    CheckDlgButton(hBtn, IDC_CHECK_SHOW_PINNED_AS_BUTTONS,
                   s.showPinnedAppsAsButtons ? BST_CHECKED : BST_UNCHECKED);

    // Separator lines
    CheckDlgButton(hBtn, IDC_CHECK_SHOW_SEPARATORS,
                   s.showSeparators ? BST_CHECKED : BST_UNCHECKED);
    InvalidateRect(GetDlgItem(hBtn, IDC_BTN_SEPARATOR_COLOR), nullptr, FALSE);
    SetSeparatorControlsEnabled(hBtn, s.showSeparators);

    // Clock tab
    CheckDlgButton(hClk, IDC_CHECK_SHOWCLOCK, s.showClock ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemTextW(hClk, IDC_EDIT_TIMEFMT, s.clockTimeFormat.c_str());
    SetDlgItemTextW(hClk, IDC_EDIT_DATEFMT, s.clockDateFormat.c_str());
    SendMessageW(GetDlgItem(hClk, IDC_SPIN_CLOCKW),       UDM_SETPOS32, 0, s.clockWidth);
    SendMessageW(GetDlgItem(hClk, IDC_SPIN_LINESPACING),  UDM_SETPOS32, 0, s.clockLineSpacing);
    SendMessageW(GetDlgItem(hClk, IDC_SPIN_TIMEFONTSIZE), UDM_SETPOS32, 0, s.clockTimeFontSize);
    SendMessageW(GetDlgItem(hClk, IDC_SPIN_DATEFONTSIZE), UDM_SETPOS32, 0, s.clockDateFontSize);
    InvalidateRect(GetDlgItem(hClk, IDC_BTN_TIMECOLOR), nullptr, FALSE);
    InvalidateRect(GetDlgItem(hClk, IDC_BTN_DATECOLOR), nullptr, FALSE);
    SetClockControlsEnabled(hClk, s.showClock);

    // App Menu tab — controls live in hScrollHosts[4] after WM_INITDIALOG reparents them
    SendMessageW(GetDlgItem(hAm, IDC_COMBO_APPMENU_LAYOUT), CB_SETCURSEL,
                 static_cast<WPARAM>(s.appMenuLayout), 0);
    auto setPos = [&](int spinId, int val) {
        SendMessageW(GetDlgItem(hAm, spinId), UDM_SETPOS32, 0, val);
    };
    setPos(IDC_SPIN_APPMENU_WIDTH,     s.appMenuWidth);
    setPos(IDC_SPIN_APPMENU_MAXHEIGHT, s.appMenuMaxHeight);
    setPos(IDC_SPIN_APPMENU_ENTRYH,    s.appMenuEntryHeight);
    setPos(IDC_SPIN_APPMENU_GRIDCOLS,  s.appMenuGridCols);
    setPos(IDC_SPIN_APPMENU_GRIDROWS,  s.appMenuGridRows);
    setPos(IDC_SPIN_APPMENU_LISTFS,    s.appMenuListFontSize);
    setPos(IDC_SPIN_APPMENU_GRIDFS,    s.appMenuGridFontSize);
    setPos(IDC_SPIN_APPMENU_MARGIN,    s.appMenuMargin);
    setPos(IDC_SPIN_APPMENU_PADDING,   s.appMenuPadding);
    CheckDlgButton(hAm, IDC_CHECK_APPMENU_SIDEBAR,
                   s.appMenuSidebarEnabled ? BST_CHECKED : BST_UNCHECKED);
    setPos(IDC_SPIN_APPMENU_SIDEBARW,  s.appMenuSidebarWidth);
    CheckDlgButton(hAm, IDC_CHECK_APPMENU_SIDEBAR_EXPLORER,
                   s.appMenuSidebarShowExplorer ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hAm, IDC_CHECK_APPMENU_SIDEBAR_SETTINGS,
                   s.appMenuSidebarShowSettings ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hAm, IDC_CHECK_APPMENU_SIDEBAR_POWER,
                   s.appMenuSidebarShowPower ? BST_CHECKED : BST_UNCHECKED);
    SetSidebarControlsEnabled(hAm, s.appMenuSidebarEnabled);
    CheckDlgButton(hAm, IDC_CHECK_APPMENU_FLATTEN_SUBMENUS,
                   s.appMenuFlattenMode == AppMenuFlattenMode::Submenus ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hAm, IDC_CHECK_APPMENU_FLATTEN_ALL,
                   s.appMenuFlattenMode == AppMenuFlattenMode::All ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hAm, IDC_CHECK_APPMENU_SEARCH,
                   s.appMenuSearchEnabled ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hAm, IDC_CHECK_APPMENU_SEARCH_FUZZY,
                   s.appMenuSearchFuzzy ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hAm, IDC_CHECK_APPMENU_SEARCH_SYSTEM,
                   s.appMenuSearchSystem ? BST_CHECKED : BST_UNCHECKED);
    SetSearchControlsEnabled(hAm, s.appMenuSearchEnabled);
}

INT_PTR CALLBACK SettingsDialog::DlgProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    DlgData* data = reinterpret_cast<DlgData*>(GetWindowLongPtrW(hwnd, DWLP_USER));

    switch (uMsg) {
    case WM_INITDIALOG: {
        data = reinterpret_cast<DlgData*>(lParam);
        SetWindowLongPtrW(hwnd, DWLP_USER, lParam);

        // General
        HWND hPos = GetDlgItem(hwnd, IDC_COMBO_POSITION);
        for (auto* s : kPositions) SendMessageW(hPos, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
        SendMessageW(hPos, CB_SETCURSEL, static_cast<WPARAM>(data->settings->position), 0);

        HWND hThickSpin = GetDlgItem(hwnd, IDC_SPIN_THICKNESS);
        HWND hThickEdit = GetDlgItem(hwnd, IDC_EDIT_THICKNESS);
        SendMessageW(hThickSpin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(hThickEdit), 0);
        SendMessageW(hThickSpin, UDM_SETRANGE32, 28, 120);
        SendMessageW(hThickSpin, UDM_SETPOS32, 0, static_cast<LPARAM>(data->settings->thickness));

        {
            HWND hMon = GetDlgItem(hwnd, IDC_COMBO_TASKBAR_MONITOR);
            for (auto* s : kTaskbarMonitorModes)
                SendMessageW(hMon, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
            SendMessageW(hMon, CB_SETCURSEL,
                         static_cast<WPARAM>(data->settings->taskbarMonitorMode), 0);

            HWND hHook = GetDlgItem(hwnd, IDC_COMBO_TASKBAR_HOOK);
            for (auto* s : kTaskbarHookMethods)
                SendMessageW(hHook, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
            SendMessageW(hHook, CB_SETCURSEL,
                         static_cast<WPARAM>(data->settings->taskbarHookMethod), 0);

            bool allMonitors = (data->settings->taskbarMonitorMode == TaskbarMonitorMode::AllMonitors);
            CheckDlgButton(hwnd, IDC_CHECK_APPMENU_ALL_MONITORS,
                           data->settings->showAppMenuOnAllMonitors ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_CHECK_CURRENT_MONITOR_APPS,
                           data->settings->showCurrentMonitorAppsOnly ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_CHECK_PINNED_PER_MONITOR,
                           data->settings->pinnedAppsPerMonitor ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_CHECK_TRAY_ICONS,
                           data->settings->showTrayIcons ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_CHECK_STATUS_ZONE,
                           data->settings->showStatusZone ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_CHECK_HIDE_DEFAULT_TRAY_ICONS,
                           data->settings->showWinzooCustomIcons ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_CHECK_TRAY_FALLBACK_EXE,
                           data->settings->trayIconFallbackExe ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_CHECK_TRAY_OVERFLOW,
                           data->settings->showOverflowTrayIcons ? BST_CHECKED : BST_UNCHECKED);
            {
                auto setupSpin = [&](int spinId, int editId, int lo, int hi, int val) {
                    HWND hSpin = GetDlgItem(hwnd, spinId);
                    HWND hEdit = GetDlgItem(hwnd, editId);
                    SendMessageW(hSpin, UDM_SETBUDDY,   reinterpret_cast<WPARAM>(hEdit), 0);
                    SendMessageW(hSpin, UDM_SETRANGE32, lo, hi);
                    SendMessageW(hSpin, UDM_SETPOS32,   0, static_cast<LPARAM>(val));
                };
                setupSpin(IDC_SPIN_TRAY_ICON_SIZE,    IDC_EDIT_TRAY_ICON_SIZE,    12, 48,
                          data->settings->trayIconSize);
                setupSpin(IDC_SPIN_TRAY_ICON_PADDING, IDC_EDIT_TRAY_ICON_PADDING,  0, 20,
                          data->settings->trayIconPadding);
                setupSpin(IDC_SPIN_TRAY_ICON_MARGIN,  IDC_EDIT_TRAY_ICON_MARGIN,   0, 20,
                          data->settings->trayIconMargin);
            }
            CheckDlgButton(hwnd, IDC_CHECK_OPEN_SAME_MONITOR,
                           data->settings->openAppsOnSameMonitor ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_CHECK_VERTICAL_TITLES,
                           data->settings->showTitlesOnVertical ? BST_CHECKED : BST_UNCHECKED);
            {
                HWND hVHSpin = GetDlgItem(hwnd, IDC_SPIN_VERTICAL_BTN_H);
                HWND hVHEdit = GetDlgItem(hwnd, IDC_EDIT_VERTICAL_BTN_H);
                SendMessageW(hVHSpin, UDM_SETBUDDY,   reinterpret_cast<WPARAM>(hVHEdit), 0);
                SendMessageW(hVHSpin, UDM_SETRANGE32, 20, 600);
                SendMessageW(hVHSpin, UDM_SETPOS32,   0,
                             static_cast<LPARAM>(data->settings->leftRightHeight));
            }
            SetVerticalTitleControlsEnabled(hwnd, data->settings->showTitlesOnVertical);
            SetAllMonitorsControlsEnabled(hwnd, allMonitors);
        }

        // App Buttons
        HWND hMaxSpin = GetDlgItem(hwnd, IDC_SPIN_MAXBTNW);
        HWND hMaxEdit = GetDlgItem(hwnd, IDC_EDIT_MAXBTNW);
        SendMessageW(hMaxSpin, UDM_SETBUDDY,   reinterpret_cast<WPARAM>(hMaxEdit), 0);
        SendMessageW(hMaxSpin, UDM_SETRANGE32, 48, 400);
        SendMessageW(hMaxSpin, UDM_SETPOS32,   0, static_cast<LPARAM>(data->settings->maxButtonWidth));

        HWND hMinSpin = GetDlgItem(hwnd, IDC_SPIN_MINBTNW);
        HWND hMinEdit = GetDlgItem(hwnd, IDC_EDIT_MINBTNW);
        SendMessageW(hMinSpin, UDM_SETBUDDY,   reinterpret_cast<WPARAM>(hMinEdit), 0);
        SendMessageW(hMinSpin, UDM_SETRANGE32, 24, 400);
        SendMessageW(hMinSpin, UDM_SETPOS32,   0, static_cast<LPARAM>(data->settings->minButtonWidth));

        {
            HWND hIconSpin = GetDlgItem(hwnd, IDC_SPIN_APP_BTN_ICON_SZ);
            HWND hIconEdit = GetDlgItem(hwnd, IDC_EDIT_APP_BTN_ICON_SZ);
            SendMessageW(hIconSpin, UDM_SETBUDDY,   reinterpret_cast<WPARAM>(hIconEdit), 0);
            SendMessageW(hIconSpin, UDM_SETRANGE32, 16, 64);
            SendMessageW(hIconSpin, UDM_SETPOS32,   0, static_cast<LPARAM>(data->settings->appButtonIconSize));
        }

        CheckDlgButton(hwnd, IDC_CHECK_MIDDLECLICK,
                       data->settings->middleClickClose ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_CHECK_RIGHTCLICKGAP,
                       data->settings->showRightClickGap ? BST_CHECKED : BST_UNCHECKED);

        CheckDlgButton(hwnd, IDC_CHECK_MINIMIZED_INDICATOR,
                       data->settings->showMinimizedIndicator ? BST_CHECKED : BST_UNCHECKED);

        {
            HWND hIndType = GetDlgItem(hwnd, IDC_COMBO_MINIMIZED_INDICATOR_TYPE);
            for (auto* s : kMinimizedIndicatorTypes)
                SendMessageW(hIndType, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
            SendMessageW(hIndType, CB_SETCURSEL,
                         static_cast<WPARAM>(data->settings->minimizedIndicatorType), 0);
        }

        HWND hIndWSpin = GetDlgItem(hwnd, IDC_SPIN_MINIMIZED_INDICATOR_W);
        HWND hIndWEdit = GetDlgItem(hwnd, IDC_EDIT_MINIMIZED_INDICATOR_W);
        SendMessageW(hIndWSpin, UDM_SETBUDDY,   reinterpret_cast<WPARAM>(hIndWEdit), 0);
        SendMessageW(hIndWSpin, UDM_SETRANGE32, 2, 40);
        SendMessageW(hIndWSpin, UDM_SETPOS32,   0, static_cast<LPARAM>(data->settings->minimizedIndicatorW));

        HWND hIndHSpin = GetDlgItem(hwnd, IDC_SPIN_MINIMIZED_INDICATOR_H);
        HWND hIndHEdit = GetDlgItem(hwnd, IDC_EDIT_MINIMIZED_INDICATOR_H);
        SendMessageW(hIndHSpin, UDM_SETBUDDY,   reinterpret_cast<WPARAM>(hIndHEdit), 0);
        SendMessageW(hIndHSpin, UDM_SETRANGE32, 1, 20);
        SendMessageW(hIndHSpin, UDM_SETPOS32,   0, static_cast<LPARAM>(data->settings->minimizedIndicatorH));

        SetMinimizedIndicatorControlsEnabled(hwnd, data->settings->showMinimizedIndicator);

        CheckDlgButton(hwnd, IDC_CHECK_PINNED_AS_BUTTONS,
                       data->settings->pinnedAppsAsButtonsWhenOpen ? BST_CHECKED : BST_UNCHECKED);

        // Progress bars
        CheckDlgButton(hwnd, IDC_CHECK_PROGRESSBAR,
                       data->settings->showProgressBars ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_CHECK_PROGRESSBAR_THEMECLR,
                       data->settings->progressBarUseThemeColor ? BST_CHECKED : BST_UNCHECKED);
        {
            HWND hPbHSpin = GetDlgItem(hwnd, IDC_SPIN_PROGRESSBAR_HEIGHT);
            HWND hPbHEdit = GetDlgItem(hwnd, IDC_EDIT_PROGRESSBAR_HEIGHT);
            SendMessageW(hPbHSpin, UDM_SETBUDDY,   reinterpret_cast<WPARAM>(hPbHEdit), 0);
            SendMessageW(hPbHSpin, UDM_SETRANGE32, 1, 10);
            SendMessageW(hPbHSpin, UDM_SETPOS32,   0,
                         static_cast<LPARAM>(data->settings->progressBarHeight));
        }
        SetProgressBarControlsEnabled(hwnd, data->settings->showProgressBars,
                                      data->settings->progressBarUseThemeColor);

        {
            HWND hRadSpin = GetDlgItem(hwnd, IDC_SPIN_BTN_OUTLINE_RADIUS);
            HWND hRadEdit = GetDlgItem(hwnd, IDC_EDIT_BTN_OUTLINE_RADIUS);
            SendMessageW(hRadSpin, UDM_SETBUDDY,   reinterpret_cast<WPARAM>(hRadEdit), 0);
            SendMessageW(hRadSpin, UDM_SETRANGE32, 0, 32);
            SendMessageW(hRadSpin, UDM_SETPOS32,   0,
                         static_cast<LPARAM>(data->settings->buttonOutlineRadius));
        }
        CheckDlgButton(hwnd, IDC_CHECK_SHOW_PINNED_AS_BUTTONS,
                       data->settings->showPinnedAppsAsButtons ? BST_CHECKED : BST_UNCHECKED);

        // Separator lines
        CheckDlgButton(hwnd, IDC_CHECK_SHOW_SEPARATORS,
                       data->settings->showSeparators ? BST_CHECKED : BST_UNCHECKED);
        SetSeparatorControlsEnabled(hwnd, data->settings->showSeparators);

        // System Clock
        CheckDlgButton(hwnd, IDC_CHECK_SHOWCLOCK,
                       data->settings->showClock ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemTextW(hwnd, IDC_EDIT_TIMEFMT, data->settings->clockTimeFormat.c_str());
        SetDlgItemTextW(hwnd, IDC_EDIT_DATEFMT, data->settings->clockDateFormat.c_str());

        HWND hClockSpin = GetDlgItem(hwnd, IDC_SPIN_CLOCKW);
        HWND hClockEdit = GetDlgItem(hwnd, IDC_EDIT_CLOCKW);
        SendMessageW(hClockSpin, UDM_SETBUDDY,   reinterpret_cast<WPARAM>(hClockEdit), 0);
        SendMessageW(hClockSpin, UDM_SETRANGE32, 40, 400);
        SendMessageW(hClockSpin, UDM_SETPOS32,   0, static_cast<LPARAM>(data->settings->clockWidth));

        HWND hSpacingSpin = GetDlgItem(hwnd, IDC_SPIN_LINESPACING);
        HWND hSpacingEdit = GetDlgItem(hwnd, IDC_EDIT_LINESPACING);
        SendMessageW(hSpacingSpin, UDM_SETBUDDY,   reinterpret_cast<WPARAM>(hSpacingEdit), 0);
        SendMessageW(hSpacingSpin, UDM_SETRANGE32, 0, 20);
        SendMessageW(hSpacingSpin, UDM_SETPOS32,   0, static_cast<LPARAM>(data->settings->clockLineSpacing));

        HWND hTimeSzSpin = GetDlgItem(hwnd, IDC_SPIN_TIMEFONTSIZE);
        HWND hTimeSzEdit = GetDlgItem(hwnd, IDC_EDIT_TIMEFONTSIZE);
        SendMessageW(hTimeSzSpin, UDM_SETBUDDY,   reinterpret_cast<WPARAM>(hTimeSzEdit), 0);
        SendMessageW(hTimeSzSpin, UDM_SETRANGE32, 6, 36);
        SendMessageW(hTimeSzSpin, UDM_SETPOS32,   0, static_cast<LPARAM>(data->settings->clockTimeFontSize));

        HWND hDateSzSpin = GetDlgItem(hwnd, IDC_SPIN_DATEFONTSIZE);
        HWND hDateSzEdit = GetDlgItem(hwnd, IDC_EDIT_DATEFONTSIZE);
        SendMessageW(hDateSzSpin, UDM_SETBUDDY,   reinterpret_cast<WPARAM>(hDateSzEdit), 0);
        SendMessageW(hDateSzSpin, UDM_SETRANGE32, 6, 36);
        SendMessageW(hDateSzSpin, UDM_SETPOS32,   0, static_cast<LPARAM>(data->settings->clockDateFontSize));

        SetClockControlsEnabled(hwnd, data->settings->showClock);

        // Tab control
        HWND hTab = GetDlgItem(hwnd, IDC_TAB_SETTINGS);
        TCITEMW tci = {};
        tci.mask = TCIF_TEXT;
        wchar_t t0[] = L"General",    t1[] = L"Visual",
                t2[] = L"App Buttons", t3[] = L"System Clock",
                t4[] = L"App Menu",   t5[] = L"System Tray";
        tci.pszText = t0; TabCtrl_InsertItem(hTab, 0, &tci);
        tci.pszText = t1; TabCtrl_InsertItem(hTab, 1, &tci);
        tci.pszText = t2; TabCtrl_InsertItem(hTab, 2, &tci);
        tci.pszText = t3; TabCtrl_InsertItem(hTab, 3, &tci);
        tci.pszText = t4; TabCtrl_InsertItem(hTab, 4, &tci);
        tci.pszText = t5; TabCtrl_InsertItem(hTab, 5, &tci);

        // Disable visual styles on checkboxes so they respect the transparent
        // background brush from WM_CTLCOLORBTN instead of painting their own.
        static const int kCheckIds[] = {
            IDC_CHECK_MIDDLECLICK, IDC_CHECK_RIGHTCLICKGAP, IDC_CHECK_SHOWCLOCK,
            IDC_CHECK_MINIMIZED_INDICATOR, IDC_CHECK_PINNED_AS_BUTTONS,
            IDC_CHECK_PROGRESSBAR, IDC_CHECK_PROGRESSBAR_THEMECLR,
            IDC_CHECK_SHOW_SEPARATORS,
            IDC_CHECK_APPMENU_ALL_MONITORS, IDC_CHECK_CURRENT_MONITOR_APPS,
            IDC_CHECK_PINNED_PER_MONITOR,
            IDC_CHECK_STATUS_ZONE, IDC_CHECK_TRAY_ICONS, IDC_CHECK_HIDE_DEFAULT_TRAY_ICONS,
            IDC_CHECK_TRAY_FALLBACK_EXE, IDC_CHECK_TRAY_OVERFLOW,
            IDC_CHECK_OPEN_SAME_MONITOR,
            IDC_CHECK_VERTICAL_TITLES,
            IDC_CHECK_APPMENU_SIDEBAR,
            IDC_CHECK_APPMENU_SIDEBAR_EXPLORER,
            IDC_CHECK_APPMENU_SIDEBAR_SETTINGS,
            IDC_CHECK_APPMENU_SIDEBAR_POWER,
            IDC_CHECK_APPMENU_FLATTEN_SUBMENUS,
            IDC_CHECK_APPMENU_FLATTEN_ALL,
            IDC_CHECK_APPMENU_SEARCH,
            IDC_CHECK_APPMENU_SEARCH_FUZZY,
            IDC_CHECK_APPMENU_SEARCH_SYSTEM,
            IDC_CHECK_APPMENU_CLASSIC_DOCS,
            IDC_CHECK_APPMENU_CLASSIC_PICS,
            IDC_CHECK_APPMENU_CLASSIC_MUSIC,
            IDC_CHECK_APPMENU_CLASSIC_DLOADS,
            IDC_CHECK_APPMENU_CLASSIC_RECENT,
            IDC_CHECK_APPMENU_CLASSIC_THISPC,
            IDC_CHECK_APPMENU_CLASSIC_CTRL,
            IDC_CHECK_APPMENU_CLASSIC_WINSETT,
            IDC_CHECK_APPMENU_CLASSIC_RUN,
            IDC_CHECK_APPMENU_CLASSIC_SHUTDOWN,
            IDC_CHECK_SHOW_PINNED_AS_BUTTONS,
            0
        };
        for (const int* id = kCheckIds; *id; ++id)
            SetWindowTheme(GetDlgItem(hwnd, *id), L"", L"");

        // App Menu tab init
        {
            HWND hLayout = GetDlgItem(hwnd, IDC_COMBO_APPMENU_LAYOUT);
            for (auto* s : kAppMenuLayouts)
                SendMessageW(hLayout, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
            SendMessageW(hLayout, CB_SETCURSEL, static_cast<WPARAM>(data->settings->appMenuLayout), 0);

            auto initSpin = [&](int spinId, int editId, int lo, int hi, int val) {
                HWND hSpin = GetDlgItem(hwnd, spinId);
                HWND hEdit = GetDlgItem(hwnd, editId);
                SendMessageW(hSpin, UDM_SETBUDDY,   reinterpret_cast<WPARAM>(hEdit), 0);
                SendMessageW(hSpin, UDM_SETRANGE32, lo, hi);
                SendMessageW(hSpin, UDM_SETPOS32,   0, val);
            };

            initSpin(IDC_SPIN_APPMENU_WIDTH,     IDC_EDIT_APPMENU_WIDTH,     120,  800, data->settings->appMenuWidth);
            initSpin(IDC_SPIN_APPMENU_MAXHEIGHT, IDC_EDIT_APPMENU_MAXHEIGHT, 100, 2000, data->settings->appMenuMaxHeight);
            initSpin(IDC_SPIN_APPMENU_ENTRYH,    IDC_EDIT_APPMENU_ENTRYH,     20,   80, data->settings->appMenuEntryHeight);
            initSpin(IDC_SPIN_APPMENU_GRIDCOLS,  IDC_EDIT_APPMENU_GRIDCOLS,    1,   12, data->settings->appMenuGridCols);
            initSpin(IDC_SPIN_APPMENU_GRIDROWS,  IDC_EDIT_APPMENU_GRIDROWS,    1,   20, data->settings->appMenuGridRows);
            initSpin(IDC_SPIN_APPMENU_LISTFS,    IDC_EDIT_APPMENU_LISTFS,      6,   36, data->settings->appMenuListFontSize);
            initSpin(IDC_SPIN_APPMENU_GRIDFS,    IDC_EDIT_APPMENU_GRIDFS,      6,   36, data->settings->appMenuGridFontSize);
            initSpin(IDC_SPIN_APPMENU_MARGIN,    IDC_EDIT_APPMENU_MARGIN,      0,   40, data->settings->appMenuMargin);
            initSpin(IDC_SPIN_APPMENU_PADDING,   IDC_EDIT_APPMENU_PADDING,     0,   40, data->settings->appMenuPadding);

            CheckDlgButton(hwnd, IDC_CHECK_APPMENU_SIDEBAR,
                           data->settings->appMenuSidebarEnabled ? BST_CHECKED : BST_UNCHECKED);
            initSpin(IDC_SPIN_APPMENU_SIDEBARW,  IDC_EDIT_APPMENU_SIDEBARW,    4,  120, data->settings->appMenuSidebarWidth);
            CheckDlgButton(hwnd, IDC_CHECK_APPMENU_SIDEBAR_EXPLORER,
                           data->settings->appMenuSidebarShowExplorer ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_CHECK_APPMENU_SIDEBAR_SETTINGS,
                           data->settings->appMenuSidebarShowSettings ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_CHECK_APPMENU_SIDEBAR_POWER,
                           data->settings->appMenuSidebarShowPower ? BST_CHECKED : BST_UNCHECKED);
            SetSidebarControlsEnabled(hwnd, data->settings->appMenuSidebarEnabled);

            CheckDlgButton(hwnd, IDC_CHECK_APPMENU_FLATTEN_SUBMENUS,
                           data->settings->appMenuFlattenMode == AppMenuFlattenMode::Submenus
                           ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_CHECK_APPMENU_FLATTEN_ALL,
                           data->settings->appMenuFlattenMode == AppMenuFlattenMode::All
                           ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_CHECK_APPMENU_SEARCH,
                           data->settings->appMenuSearchEnabled ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_CHECK_APPMENU_SEARCH_FUZZY,
                           data->settings->appMenuSearchFuzzy ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_CHECK_APPMENU_SEARCH_SYSTEM,
                           data->settings->appMenuSearchSystem ? BST_CHECKED : BST_UNCHECKED);
            SetSearchControlsEnabled(hwnd, data->settings->appMenuSearchEnabled);

            initSpin(IDC_SPIN_APPMENU_CLASSIC_PW, IDC_EDIT_APPMENU_CLASSIC_PW, 80, 400,
                     data->settings->appMenuClassicPanelWidth);
            CheckDlgButton(hwnd, IDC_CHECK_APPMENU_CLASSIC_DOCS,
                           data->settings->appMenuClassicShowDocuments   ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_CHECK_APPMENU_CLASSIC_PICS,
                           data->settings->appMenuClassicShowPictures    ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_CHECK_APPMENU_CLASSIC_MUSIC,
                           data->settings->appMenuClassicShowMusic       ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_CHECK_APPMENU_CLASSIC_DLOADS,
                           data->settings->appMenuClassicShowDownloads   ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_CHECK_APPMENU_CLASSIC_RECENT,
                           data->settings->appMenuClassicShowRecentItems ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_CHECK_APPMENU_CLASSIC_THISPC,
                           data->settings->appMenuClassicShowThisPC      ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_CHECK_APPMENU_CLASSIC_CTRL,
                           data->settings->appMenuClassicShowControlPanel? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_CHECK_APPMENU_CLASSIC_WINSETT,
                           data->settings->appMenuClassicShowWinSettings ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_CHECK_APPMENU_CLASSIC_RUN,
                           data->settings->appMenuClassicShowRun         ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_CHECK_APPMENU_CLASSIC_SHUTDOWN,
                           data->settings->appMenuClassicShowShutDown    ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_CHECK_APPMENU_PINNED_PER_MONITOR,
                           data->settings->appMenuPinnedPerMonitor       ? BST_CHECKED : BST_UNCHECKED);
        }

        // Build scrollable hosts for all tabs
        {
            RegisterScrollHostClass();

            HWND hTabCtrl = GetDlgItem(hwnd, IDC_TAB_SETTINGS);
            RECT tabWndRect;
            GetWindowRect(hTabCtrl, &tabWndRect);
            RECT content = tabWndRect;
            TabCtrl_AdjustRect(hTabCtrl, FALSE, &content);
            MapWindowPoints(HWND_DESKTOP, hwnd, reinterpret_cast<LPPOINT>(&content), 2);

            int panelW = content.right  - content.left;
            int panelH = content.bottom - content.top;

            for (int g = 0; g < 6; ++g) {
                if (g == 1) continue; // Visual built separately below
                data->hScrollHosts[g] = CreateTabScrollHost(hwnd, kTabGroups[g],
                    content.left, content.top, panelW, panelH);
            }

            // Visual tab (index 1): scroll host created directly; controls added dynamically.
            data->hScrollHosts[1] = CreateWindowExW(
                WS_EX_CONTROLPARENT, kScrollHostClass, nullptr,
                WS_CHILD | WS_VSCROLL,
                content.left, content.top, panelW, panelH,
                hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            SetupVisualTab(hwnd, data, panelW, panelH);
        }

        // Capture layout metrics for WM_SIZE
        {
            RECT cr;
            GetClientRect(hwnd, &cr);
            int cW = cr.right, cH = cr.bottom;

            HWND hTabForMetrics = GetDlgItem(hwnd, IDC_TAB_SETTINGS);
            RECT tabR;
            GetWindowRect(hTabForMetrics, &tabR);
            MapWindowPoints(HWND_DESKTOP, hwnd, reinterpret_cast<LPPOINT>(&tabR), 2);
            data->tabL        = tabR.left;
            data->tabT        = tabR.top;
            data->tabRightGap = cW - tabR.right;
            data->tabBotGap   = cH - tabR.bottom;

            auto measureBtn = [&](int id, int& outL, int& outW, int& outRightGap, int& outH, int& outBotGap) {
                HWND hB = GetDlgItem(hwnd, id);
                RECT bR;
                GetWindowRect(hB, &bR);
                MapWindowPoints(HWND_DESKTOP, hwnd, reinterpret_cast<LPPOINT>(&bR), 2);
                outL        = bR.left;
                outW        = bR.right - bR.left;
                outRightGap = cW - bR.right;
                outH        = bR.bottom - bR.top;
                outBotGap   = cH - bR.bottom;
            };

            int dummy = 0;
            measureBtn(IDC_BTN_RESET_DEFAULTS, data->btnDefL,  data->btnDefW,  dummy, data->btnH, data->btnBotGap);
            measureBtn(IDOK,             dummy, data->btnOKW,  data->btnOKRightGap,  dummy, dummy);
            measureBtn(IDCANCEL,         dummy, data->btnCxlW, data->btnCxlRightGap, dummy, dummy);

            // Minimum size: original dialog size (in window coords)
            RECT wr;
            GetWindowRect(hwnd, &wr);
            data->minW = wr.right - wr.left;
            data->minH = wr.bottom - wr.top;
            data->layoutReady = true;
        }

        // Restore persisted size/position if valid
        {
            Settings* s = data->settings;
            if (s->settingsDlgW > 0) {
                RECT r{ s->settingsDlgX, s->settingsDlgY,
                        s->settingsDlgX + s->settingsDlgW,
                        s->settingsDlgY + s->settingsDlgH };
                if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
                    SetWindowPos(hwnd, nullptr,
                                 s->settingsDlgX, s->settingsDlgY,
                                 s->settingsDlgW, s->settingsDlgH,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                }
            }
        }

        // ShowTab must run last so that UDM_SETBUDDY calls (which make edit buddies
        // visible) are all done before we hide controls belonging to inactive tabs.
        ShowTab(hwnd, 0, data);

        return TRUE;
    }

    case WM_GETMINMAXINFO: {
        if (data && data->layoutReady) {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize = { data->minW, data->minH };
        }
        break;
    }

    case WM_SIZE: {
        if (!data || !data->layoutReady) break;

        RECT cr;
        GetClientRect(hwnd, &cr);
        int cW = cr.right, cH = cr.bottom;

        HWND hTabCtrl2 = GetDlgItem(hwnd, IDC_TAB_SETTINGS);
        int newTabW = cW - data->tabL - data->tabRightGap;
        int newTabH = cH - data->tabT - data->tabBotGap;

        // Compute scroll-host content rect inside the resized tab control
        RECT tabR{ data->tabL, data->tabT, data->tabL + newTabW, data->tabT + newTabH };
        RECT content = tabR;
        TabCtrl_AdjustRect(hTabCtrl2, FALSE, &content);
        int panelW = content.right  - content.left;
        int panelH = content.bottom - content.top;

        HDWP hdwp = BeginDeferWindowPos(1 + 6 + 3);  // 1 tab + 6 hosts + 3 buttons

        // Resize tab control
        hdwp = DeferWindowPos(hdwp, hTabCtrl2, nullptr,
                              data->tabL, data->tabT, newTabW, newTabH,
                              SWP_NOZORDER | SWP_NOACTIVATE);

        // Resize each scroll host
        for (int g = 0; g < 6; ++g) {
            if (data->hScrollHosts[g])
                hdwp = DeferWindowPos(hdwp, data->hScrollHosts[g], nullptr,
                                      content.left, content.top, panelW, panelH,
                                      SWP_NOZORDER | SWP_NOACTIVATE);
        }

        // Move buttons (keep their sizes, move by anchoring to bottom/right/left)
        int btnY = cH - data->btnBotGap - data->btnH;
        hdwp = DeferWindowPos(hdwp, GetDlgItem(hwnd, IDC_BTN_RESET_DEFAULTS), nullptr,
                              data->btnDefL, btnY, data->btnDefW, data->btnH,
                              SWP_NOZORDER | SWP_NOACTIVATE);
        hdwp = DeferWindowPos(hdwp, GetDlgItem(hwnd, IDOK), nullptr,
                              cW - data->btnOKRightGap - data->btnOKW, btnY,
                              data->btnOKW, data->btnH,
                              SWP_NOZORDER | SWP_NOACTIVATE);
        hdwp = DeferWindowPos(hdwp, GetDlgItem(hwnd, IDCANCEL), nullptr,
                              cW - data->btnCxlRightGap - data->btnCxlW, btnY,
                              data->btnCxlW, data->btnH,
                              SWP_NOZORDER | SWP_NOACTIVATE);
        EndDeferWindowPos(hdwp);

        // Update scrollbar page size and clamp scroll position for each host
        for (int g = 0; g < 6; ++g) {
            HWND hH = data->hScrollHosts[g];
            if (!hH) continue;
            SCROLLINFO si = {};
            si.cbSize = sizeof(si);
            si.fMask = SIF_ALL;
            GetScrollInfo(hH, SB_VERT, &si);
            si.nPage = static_cast<UINT>(panelH);
            if (std::cmp_greater_equal(si.nPage, si.nMax + 1)) {
                // Everything fits: scroll back to top
                int delta = -si.nPos;
                if (delta != 0) {
                    RECT hcr;
                    GetClientRect(hH, &hcr);
                    ScrollWindow(hH, 0, delta, nullptr, nullptr);
                }
                si.nPos = 0;
            } else {
                int maxPos = si.nMax - static_cast<int>(si.nPage) + 1;
                if (si.nPos > maxPos) {
                    int delta = maxPos - si.nPos;
                    ScrollWindow(hH, 0, delta, nullptr, nullptr);
                    si.nPos = maxPos;
                }
            }
            si.fMask = SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
            SetScrollInfo(hH, SB_VERT, &si, TRUE);
        }
        return 0;
    }

    case WM_DESTROY: {
        if (data) {
            RECT wr;
            GetWindowRect(hwnd, &wr);
            data->settings->settingsDlgX = wr.left;
            data->settings->settingsDlgY = wr.top;
            data->settings->settingsDlgW = wr.right  - wr.left;
            data->settings->settingsDlgH = wr.bottom - wr.top;
            // Write directly — Cancel path never calls SaveSettings
            RegistryKey key = RegistryKey::OpenAppKey(KEY_WRITE);
            if (key.IsOpen()) {
                key.WriteDword(L"SettingsDlgX", static_cast<DWORD>(data->settings->settingsDlgX));
                key.WriteDword(L"SettingsDlgY", static_cast<DWORD>(data->settings->settingsDlgY));
                key.WriteDword(L"SettingsDlgW", static_cast<DWORD>(data->settings->settingsDlgW));
                key.WriteDword(L"SettingsDlgH", static_cast<DWORD>(data->settings->settingsDlgH));
            }
        }
        break;
    }

    case WM_NOTIFY: {
        auto* nm = reinterpret_cast<NMHDR*>(lParam);
        if (data && nm->idFrom == IDC_TAB_SETTINGS && nm->code == TCN_SELCHANGE) {
            int tab = TabCtrl_GetCurSel(GetDlgItem(hwnd, IDC_TAB_SETTINGS));
            ShowTab(hwnd, tab, data);
            if (tab == 3) {  // System Clock is now tab 3
                HWND hClk = TabHost(data, 3, hwnd);
                bool on = IsDlgButtonChecked(hClk, IDC_CHECK_SHOWCLOCK) == BST_CHECKED;
                SetClockControlsEnabled(hClk, on);
            }
            return TRUE;
        }
        break;
    }

    case WM_MOUSEWHEEL:
        if (data) {
            for (int g = 0; g < 6; ++g) {
                HWND hH = data->hScrollHosts[g];
                if (hH && IsWindowVisible(hH)) {
                    SendMessageW(hH, WM_MOUSEWHEEL, wParam, lParam);
                    return TRUE;
                }
            }
        }
        break;

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, TRANSPARENT);
        return reinterpret_cast<INT_PTR>(GetStockObject(NULL_BRUSH));
    }

    case WM_DRAWITEM: {
        auto* di = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (!data) break;

        // Theme swatch buttons
        if (di->CtlID >= IDC_THEME_SWATCH_BASE &&
            di->CtlID <  IDC_THEME_SWATCH_BASE + kThemePresetCount)
        {
            int idx      = di->CtlID - IDC_THEME_SWATCH_BASE;
            bool selected = (data->selectedTheme == idx);
            bool hot      = (di->itemState & ODS_HOTLIGHT) != 0;

            const ThemeBase& tb = GetThemeBase(static_cast<ThemePreset>(idx));
            RECT rc = di->rcItem;
            // int  w  = rc.right  - rc.left;
            int  h  = rc.bottom - rc.top;

            // Name text height (bottom strip)
            int nameH  = h / 4;
            int colorH = h - nameH;

            // Top 2/3 of color block = taskbar color
            RECT topR = { rc.left, rc.top, rc.right, rc.top + colorH * 2 / 3 };
            HBRUSH brTop = CreateSolidBrush(tb.taskbar);
            FillRect(di->hDC, &topR, brTop);
            DeleteObject(brTop);

            // Bottom 1/3 of color block = accent color
            RECT accR = { rc.left, topR.bottom, rc.right, rc.top + colorH };
            HBRUSH brAcc = CreateSolidBrush(tb.accent);
            FillRect(di->hDC, &accR, brAcc);
            DeleteObject(brAcc);

            // Name strip (system background)
            RECT nameR = { rc.left, rc.top + colorH, rc.right, rc.bottom };
            FillRect(di->hDC, &nameR, GetSysColorBrush(COLOR_BTNFACE));

            // Draw name text
            HFONT hf = reinterpret_cast<HFONT>(SendMessageW(di->hwndItem, WM_GETFONT, 0, 0));
            HFONT hfOld = hf ? static_cast<HFONT>(SelectObject(di->hDC, hf)) : nullptr;
            SetBkMode(di->hDC, TRANSPARENT);
            SetTextColor(di->hDC, GetSysColor(COLOR_BTNTEXT));
            DrawTextW(di->hDC, tb.name, -1, &nameR,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            if (hfOld) SelectObject(di->hDC, hfOld);

            // Selection or hover border
            COLORREF hlClr = GetSysColor(COLOR_HIGHLIGHT);
            int penW = selected ? 2 : 1;
            if (selected || hot) {
                HPEN hPen    = CreatePen(PS_SOLID, penW, hlClr);
                HPEN hPenOld = static_cast<HPEN>(SelectObject(di->hDC, hPen));
                HBRUSH hBrOld= static_cast<HBRUSH>(SelectObject(di->hDC, GetStockObject(NULL_BRUSH)));
                Rectangle(di->hDC, rc.left, rc.top, rc.right, rc.bottom);
                SelectObject(di->hDC, hPenOld);
                SelectObject(di->hDC, hBrOld);
                DeleteObject(hPen);
            }
            return TRUE;
        }

        // Custom taskbar color picker button
        if (di->CtlID == IDC_BTN_CUSTOM_TASKBAR_COLOR) {
            COLORREF color = data->settings->customTaskbarColor;
            HBRUSH br = CreateSolidBrush(color);
            FillRect(di->hDC, &di->rcItem, br);
            DeleteObject(br);
            HPEN pen    = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_WINDOWFRAME));
            HPEN oldPen = static_cast<HPEN>(SelectObject(di->hDC, pen));
            SelectObject(di->hDC, GetStockObject(NULL_BRUSH));
            Rectangle(di->hDC, di->rcItem.left, di->rcItem.top,
                      di->rcItem.right, di->rcItem.bottom);
            SelectObject(di->hDC, oldPen);
            DeleteObject(pen);
            return TRUE;
        }

        // Other color picker buttons (clock, progress bar, separator)
        if (di->CtlID == IDC_BTN_TIMECOLOR || di->CtlID == IDC_BTN_DATECOLOR
            || di->CtlID == IDC_BTN_PROGRESSBAR_COLOR || di->CtlID == IDC_BTN_SEPARATOR_COLOR)
        {
            COLORREF color;
            if (di->CtlID == IDC_BTN_TIMECOLOR)
                color = data->settings->clockTimeColor;
            else if (di->CtlID == IDC_BTN_DATECOLOR)
                color = data->settings->clockDateColor;
            else if (di->CtlID == IDC_BTN_PROGRESSBAR_COLOR)
                color = data->settings->progressBarColor;
            else
                color = data->settings->separatorColor;
            HBRUSH br = CreateSolidBrush(color);
            FillRect(di->hDC, &di->rcItem, br);
            DeleteObject(br);
            HPEN pen    = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_WINDOWFRAME));
            HPEN oldPen = static_cast<HPEN>(SelectObject(di->hDC, pen));
            SelectObject(di->hDC, GetStockObject(NULL_BRUSH));
            Rectangle(di->hDC, di->rcItem.left, di->rcItem.top,
                      di->rcItem.right, di->rcItem.bottom);
            SelectObject(di->hDC, oldPen);
            DeleteObject(pen);
            return TRUE;
        }
        break;
    }

    case WM_COMMAND:
        // Theme swatch clicked — update selection and redraw
        if (LOWORD(wParam) >= IDC_THEME_SWATCH_BASE &&
            LOWORD(wParam) <  IDC_THEME_SWATCH_BASE + kThemePresetCount && data)
        {
            data->selectedTheme = LOWORD(wParam) - IDC_THEME_SWATCH_BASE;
            InvalidateRect(TabHost(data, 1, hwnd), nullptr, TRUE);
            return TRUE;
        }
        // Custom taskbar color: checkbox toggle
        if (LOWORD(wParam) == IDC_CHECK_CUSTOM_TASKBAR_COLOR && data) {
            HWND hVis = TabHost(data, 1, hwnd);
            bool on = IsDlgButtonChecked(hVis, IDC_CHECK_CUSTOM_TASKBAR_COLOR) == BST_CHECKED;
            EnableWindow(GetDlgItem(hVis, IDC_BTN_CUSTOM_TASKBAR_COLOR), on ? TRUE : FALSE);
            return TRUE;
        }
        // Custom taskbar color: color picker
        if (LOWORD(wParam) == IDC_BTN_CUSTOM_TASKBAR_COLOR && data) {
            static COLORREF customTaskbarCustomColors[16] = {};
            CHOOSECOLORW cc    = {};
            cc.lStructSize     = sizeof(cc);
            cc.hwndOwner       = hwnd;
            cc.rgbResult       = data->settings->customTaskbarColor;
            cc.lpCustColors    = customTaskbarCustomColors;
            cc.Flags           = CC_RGBINIT | CC_FULLOPEN;
            if (ChooseColorW(&cc)) {
                data->settings->customTaskbarColor = cc.rgbResult;
                InvalidateRect(GetDlgItem(TabHost(data, 1, hwnd), IDC_BTN_CUSTOM_TASKBAR_COLOR),
                               nullptr, FALSE);
            }
            return TRUE;
        }
        if ((LOWORD(wParam) == IDC_BTN_TIMECOLOR || LOWORD(wParam) == IDC_BTN_DATECOLOR
             || LOWORD(wParam) == IDC_BTN_PROGRESSBAR_COLOR || LOWORD(wParam) == IDC_BTN_SEPARATOR_COLOR)
            && data)
        {
            COLORREF* colorField;
            HWND hTab;
            if (LOWORD(wParam) == IDC_BTN_TIMECOLOR) {
                colorField = &data->settings->clockTimeColor;
                hTab = TabHost(data, 3, hwnd);
            } else if (LOWORD(wParam) == IDC_BTN_DATECOLOR) {
                colorField = &data->settings->clockDateColor;
                hTab = TabHost(data, 3, hwnd);
            } else if (LOWORD(wParam) == IDC_BTN_PROGRESSBAR_COLOR) {
                colorField = &data->settings->progressBarColor;
                hTab = TabHost(data, 2, hwnd);
            } else {
                colorField = &data->settings->separatorColor;
                hTab = TabHost(data, 2, hwnd);
            }
            static COLORREF customColors[16] = {};
            CHOOSECOLORW cc    = {};
            cc.lStructSize     = sizeof(cc);
            cc.hwndOwner       = hwnd;
            cc.rgbResult       = *colorField;
            cc.lpCustColors    = customColors;
            cc.Flags           = CC_RGBINIT | CC_FULLOPEN;
            if (ChooseColorW(&cc)) {
                *colorField = cc.rgbResult;
                InvalidateRect(GetDlgItem(hTab, LOWORD(wParam)), nullptr, FALSE);
            }
            return TRUE;
        }
        if (LOWORD(wParam) == IDC_CHECK_PROGRESSBAR && data) {
            HWND hBtn  = TabHost(data, 2, hwnd);
            bool pb    = IsDlgButtonChecked(hBtn, IDC_CHECK_PROGRESSBAR) == BST_CHECKED;
            bool theme = IsDlgButtonChecked(hBtn, IDC_CHECK_PROGRESSBAR_THEMECLR) == BST_CHECKED;
            SetProgressBarControlsEnabled(hBtn, pb, theme);
            return TRUE;
        }
        if (LOWORD(wParam) == IDC_CHECK_PROGRESSBAR_THEMECLR && data) {
            HWND hBtn  = TabHost(data, 2, hwnd);
            bool pb    = IsDlgButtonChecked(hBtn, IDC_CHECK_PROGRESSBAR) == BST_CHECKED;
            bool theme = IsDlgButtonChecked(hBtn, IDC_CHECK_PROGRESSBAR_THEMECLR) == BST_CHECKED;
            SetProgressBarControlsEnabled(hBtn, pb, theme);
            return TRUE;
        }
        if (LOWORD(wParam) == IDC_CHECK_SHOW_SEPARATORS && data) {
            HWND hBtn = TabHost(data, 2, hwnd);
            bool on = IsDlgButtonChecked(hBtn, IDC_CHECK_SHOW_SEPARATORS) == BST_CHECKED;
            SetSeparatorControlsEnabled(hBtn, on);
            return TRUE;
        }
        if (LOWORD(wParam) == IDC_CHECK_SHOWCLOCK) {
            HWND hClk = TabHost(data, 3, hwnd);
            bool checked = IsDlgButtonChecked(hClk, IDC_CHECK_SHOWCLOCK) == BST_CHECKED;
            SetClockControlsEnabled(hClk, checked);
            return TRUE;
        }
        if (LOWORD(wParam) == IDC_CHECK_VERTICAL_TITLES && data) {
            HWND hGen = TabHost(data, 0, hwnd);
            bool checked = IsDlgButtonChecked(hGen, IDC_CHECK_VERTICAL_TITLES) == BST_CHECKED;
            SetVerticalTitleControlsEnabled(hGen, checked);
            return TRUE;
        }
        if (LOWORD(wParam) == IDC_COMBO_TASKBAR_MONITOR
            && HIWORD(wParam) == CBN_SELCHANGE)
        {
            HWND hGen = TabHost(data, 0, hwnd);
            int sel = static_cast<int>(
                SendMessageW(GetDlgItem(hGen, IDC_COMBO_TASKBAR_MONITOR), CB_GETCURSEL, 0, 0));
            bool allMonitors = (sel == static_cast<int>(TaskbarMonitorMode::AllMonitors));
            SetAllMonitorsControlsEnabled(hGen, allMonitors);
            return TRUE;
        }
        if (LOWORD(wParam) == IDC_CHECK_MINIMIZED_INDICATOR) {
            HWND hBtn = TabHost(data, 2, hwnd);
            bool checked = IsDlgButtonChecked(hBtn, IDC_CHECK_MINIMIZED_INDICATOR) == BST_CHECKED;
            SetMinimizedIndicatorControlsEnabled(hBtn, checked);
            return TRUE;
        }
        if (LOWORD(wParam) == IDC_COMBO_MINIMIZED_INDICATOR_TYPE
            && HIWORD(wParam) == CBN_SELCHANGE)
        {
            HWND hBtn = TabHost(data, 2, hwnd);
            bool checked = IsDlgButtonChecked(hBtn, IDC_CHECK_MINIMIZED_INDICATOR) == BST_CHECKED;
            SetMinimizedIndicatorControlsEnabled(hBtn, checked);
            return TRUE;
        }
        if (LOWORD(wParam) == IDC_CHECK_APPMENU_SIDEBAR) {
            HWND hAm = TabHost(data, 4, hwnd);
            bool checked = IsDlgButtonChecked(hAm, IDC_CHECK_APPMENU_SIDEBAR) == BST_CHECKED;
            SetSidebarControlsEnabled(hAm, checked);
            return TRUE;
        }
        if (LOWORD(wParam) == IDC_CHECK_APPMENU_SEARCH) {
            HWND hAm = TabHost(data, 4, hwnd);
            bool checked = IsDlgButtonChecked(hAm, IDC_CHECK_APPMENU_SEARCH) == BST_CHECKED;
            SetSearchControlsEnabled(hAm, checked);
            return TRUE;
        }
        if (LOWORD(wParam) == IDC_CHECK_APPMENU_FLATTEN_SUBMENUS && data) {
            HWND hAm = TabHost(data, 4, hwnd);
            if (IsDlgButtonChecked(hAm, IDC_CHECK_APPMENU_FLATTEN_SUBMENUS) == BST_CHECKED)
                CheckDlgButton(hAm, IDC_CHECK_APPMENU_FLATTEN_ALL, BST_UNCHECKED);
            return TRUE;
        }
        if (LOWORD(wParam) == IDC_CHECK_APPMENU_FLATTEN_ALL && data) {
            HWND hAm = TabHost(data, 4, hwnd);
            if (IsDlgButtonChecked(hAm, IDC_CHECK_APPMENU_FLATTEN_ALL) == BST_CHECKED)
                CheckDlgButton(hAm, IDC_CHECK_APPMENU_FLATTEN_SUBMENUS, BST_UNCHECKED);
            return TRUE;
        }
        if (LOWORD(wParam) == IDC_BTN_RESET_DEFAULTS && data) {
            if (MessageBoxW(hwnd,
                            L"Reset all settings to their defaults?\n\nPinned apps will not be affected.",
                            L"Reset to Defaults",
                            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
            {
                std::vector<std::wstring> pinned = data->settings->pinnedExePaths;
                auto pinnedPerMonitor = data->settings->pinnedExePathsPerMonitor;
                bool pinnedPerMonitorFlag = data->settings->pinnedAppsPerMonitor;
                auto appMenuPinned = data->settings->appMenuPinnedPaths;
                auto appMenuPinnedPerMon = data->settings->appMenuPinnedPathsPerMonitor;
                bool appMenuPinnedPerMonFlag = data->settings->appMenuPinnedPerMonitor;
                *data->settings = Settings{};
                data->settings->pinnedExePaths = std::move(pinned);
                data->settings->pinnedExePathsPerMonitor = std::move(pinnedPerMonitor);
                data->settings->pinnedAppsPerMonitor = pinnedPerMonitorFlag;
                data->settings->appMenuPinnedPaths = std::move(appMenuPinned);
                data->settings->appMenuPinnedPathsPerMonitor = std::move(appMenuPinnedPerMon);
                data->settings->appMenuPinnedPerMonitor = appMenuPinnedPerMonFlag;
                ApplySettingsToControls(hwnd, data);
            }
            return TRUE;
        }
        if (LOWORD(wParam) == IDOK && data) {
            HWND hGen  = TabHost(data, 0, hwnd);
            HWND hVis  = TabHost(data, 1, hwnd);
            HWND hBtn  = TabHost(data, 2, hwnd);
            HWND hClk  = TabHost(data, 3, hwnd);
            HWND hAm   = TabHost(data, 4, hwnd);
            HWND hTray = TabHost(data, 5, hwnd);

            HWND hPos  = GetDlgItem(hGen, IDC_COMBO_POSITION);
            int posIdx = static_cast<int>(SendMessageW(hPos, CB_GETCURSEL, 0, 0));
            int thick  = static_cast<int>(
                SendMessageW(GetDlgItem(hGen, IDC_SPIN_THICKNESS), UDM_GETPOS32, 0, 0));

            if (posIdx >= 0)             data->settings->position  = static_cast<TaskbarPosition>(posIdx);
            if (thick >= 28 && thick <= 120) data->settings->thickness = thick;

            // Visual tab
            if (data->selectedTheme >= 0 && data->selectedTheme < kThemePresetCount)
                data->settings->theme = static_cast<ThemePreset>(data->selectedTheme);
            if (hVis) {
                data->settings->useCustomTaskbarColor =
                    IsDlgButtonChecked(hVis, IDC_CHECK_CUSTOM_TASKBAR_COLOR) == BST_CHECKED;
                // customTaskbarColor is updated immediately on color pick
            }

            {
                HWND hMon = GetDlgItem(hGen, IDC_COMBO_TASKBAR_MONITOR);
                int monIdx = static_cast<int>(SendMessageW(hMon, CB_GETCURSEL, 0, 0));
                if (monIdx >= 0)
                    data->settings->taskbarMonitorMode = static_cast<TaskbarMonitorMode>(monIdx);
                int hookIdx = static_cast<int>(
                    SendMessageW(GetDlgItem(hGen, IDC_COMBO_TASKBAR_HOOK), CB_GETCURSEL, 0, 0));
                if (hookIdx >= 0 && hookIdx <= 1)
                    data->settings->taskbarHookMethod = static_cast<TaskbarHookMethod>(hookIdx);
                data->settings->showAppMenuOnAllMonitors =
                    IsDlgButtonChecked(hGen, IDC_CHECK_APPMENU_ALL_MONITORS) == BST_CHECKED;
                data->settings->showCurrentMonitorAppsOnly =
                    IsDlgButtonChecked(hGen, IDC_CHECK_CURRENT_MONITOR_APPS) == BST_CHECKED;
                data->settings->pinnedAppsPerMonitor =
                    IsDlgButtonChecked(hGen, IDC_CHECK_PINNED_PER_MONITOR) == BST_CHECKED;
                data->settings->openAppsOnSameMonitor =
                    IsDlgButtonChecked(hGen, IDC_CHECK_OPEN_SAME_MONITOR) == BST_CHECKED;
                data->settings->showTitlesOnVertical =
                    IsDlgButtonChecked(hGen, IDC_CHECK_VERTICAL_TITLES) == BST_CHECKED;
                {
                    int vh = static_cast<int>(
                        SendMessageW(GetDlgItem(hGen, IDC_SPIN_VERTICAL_BTN_H), UDM_GETPOS32, 0, 0));
                    if (vh >= 20 && vh <= 600) data->settings->leftRightHeight = vh;
                }
            }

            // System Tray tab
            {
                data->settings->showTrayIcons =
                    IsDlgButtonChecked(hTray, IDC_CHECK_TRAY_ICONS) == BST_CHECKED;
                data->settings->trayIconFallbackExe =
                    IsDlgButtonChecked(hTray, IDC_CHECK_TRAY_FALLBACK_EXE) == BST_CHECKED;
                data->settings->showOverflowTrayIcons =
                    IsDlgButtonChecked(hTray, IDC_CHECK_TRAY_OVERFLOW) == BST_CHECKED;
                int tsz = static_cast<int>(
                    SendMessageW(GetDlgItem(hTray, IDC_SPIN_TRAY_ICON_SIZE), UDM_GETPOS32, 0, 0));
                if (tsz >= 12 && tsz <= 48) data->settings->trayIconSize = tsz;
                int tpad = static_cast<int>(
                    SendMessageW(GetDlgItem(hTray, IDC_SPIN_TRAY_ICON_PADDING), UDM_GETPOS32, 0, 0));
                if (tpad >= 0 && tpad <= 20) data->settings->trayIconPadding = tpad;
                int tmar = static_cast<int>(
                    SendMessageW(GetDlgItem(hTray, IDC_SPIN_TRAY_ICON_MARGIN), UDM_GETPOS32, 0, 0));
                if (tmar >= 0 && tmar <= 20) data->settings->trayIconMargin = tmar;
                data->settings->showStatusZone =
                    IsDlgButtonChecked(hTray, IDC_CHECK_STATUS_ZONE) == BST_CHECKED;
                data->settings->showWinzooCustomIcons =
                    IsDlgButtonChecked(hTray, IDC_CHECK_HIDE_DEFAULT_TRAY_ICONS) == BST_CHECKED;
            }

            int maxW = static_cast<int>(
                SendMessageW(GetDlgItem(hBtn, IDC_SPIN_MAXBTNW), UDM_GETPOS32, 0, 0));
            int minW = static_cast<int>(
                SendMessageW(GetDlgItem(hBtn, IDC_SPIN_MINBTNW), UDM_GETPOS32, 0, 0));
            if (maxW >= 48 && maxW <= 400) data->settings->maxButtonWidth = maxW;
            if (minW >= 24 && minW <= 400) data->settings->minButtonWidth = minW;
            if (data->settings->minButtonWidth > data->settings->maxButtonWidth)
                data->settings->minButtonWidth = data->settings->maxButtonWidth;
            int iconSz = static_cast<int>(
                SendMessageW(GetDlgItem(hBtn, IDC_SPIN_APP_BTN_ICON_SZ), UDM_GETPOS32, 0, 0));
            if (iconSz >= 16 && iconSz <= 64) data->settings->appButtonIconSize = iconSz;

            data->settings->middleClickClose =
                IsDlgButtonChecked(hBtn, IDC_CHECK_MIDDLECLICK) == BST_CHECKED;
            data->settings->showRightClickGap =
                IsDlgButtonChecked(hBtn, IDC_CHECK_RIGHTCLICKGAP) == BST_CHECKED;

            data->settings->showMinimizedIndicator =
                IsDlgButtonChecked(hBtn, IDC_CHECK_MINIMIZED_INDICATOR) == BST_CHECKED;

            {
                int typeIdx = static_cast<int>(
                    SendMessageW(GetDlgItem(hBtn, IDC_COMBO_MINIMIZED_INDICATOR_TYPE), CB_GETCURSEL, 0, 0));
                if (typeIdx >= 0)
                    data->settings->minimizedIndicatorType = static_cast<MinimizedIndicatorType>(typeIdx);
            }

            int indW = static_cast<int>(
                SendMessageW(GetDlgItem(hBtn, IDC_SPIN_MINIMIZED_INDICATOR_W), UDM_GETPOS32, 0, 0));
            int indH = static_cast<int>(
                SendMessageW(GetDlgItem(hBtn, IDC_SPIN_MINIMIZED_INDICATOR_H), UDM_GETPOS32, 0, 0));
            if (indW >= 2 && indW <= 40) data->settings->minimizedIndicatorW = indW;
            if (indH >= 1 && indH <= 20) data->settings->minimizedIndicatorH = indH;

            data->settings->pinnedAppsAsButtonsWhenOpen =
                IsDlgButtonChecked(hBtn, IDC_CHECK_PINNED_AS_BUTTONS) == BST_CHECKED;

            data->settings->showProgressBars =
                IsDlgButtonChecked(hBtn, IDC_CHECK_PROGRESSBAR) == BST_CHECKED;
            data->settings->progressBarUseThemeColor =
                IsDlgButtonChecked(hBtn, IDC_CHECK_PROGRESSBAR_THEMECLR) == BST_CHECKED;
            // progressBarColor is updated immediately on pick (like clock colors)
            {
                int pbH = static_cast<int>(
                    SendMessageW(GetDlgItem(hBtn, IDC_SPIN_PROGRESSBAR_HEIGHT), UDM_GETPOS32, 0, 0));
                if (pbH >= 1 && pbH <= 10) data->settings->progressBarHeight = pbH;
            }
            {
                int rad = static_cast<int>(
                    SendMessageW(GetDlgItem(hBtn, IDC_SPIN_BTN_OUTLINE_RADIUS), UDM_GETPOS32, 0, 0));
                if (rad >= 0 && rad <= 32) data->settings->buttonOutlineRadius = rad;
            }
            data->settings->showPinnedAppsAsButtons =
                IsDlgButtonChecked(hBtn, IDC_CHECK_SHOW_PINNED_AS_BUTTONS) == BST_CHECKED;

            data->settings->showSeparators =
                IsDlgButtonChecked(hBtn, IDC_CHECK_SHOW_SEPARATORS) == BST_CHECKED;
            // separatorColor is updated immediately on pick

            data->settings->showClock =
                IsDlgButtonChecked(hClk, IDC_CHECK_SHOWCLOCK) == BST_CHECKED;

            wchar_t buf[128];
            GetDlgItemTextW(hClk, IDC_EDIT_TIMEFMT, buf, 128);
            data->settings->clockTimeFormat = buf;
            GetDlgItemTextW(hClk, IDC_EDIT_DATEFMT, buf, 128);
            data->settings->clockDateFormat = buf;

            int cw = static_cast<int>(
                SendMessageW(GetDlgItem(hClk, IDC_SPIN_CLOCKW), UDM_GETPOS32, 0, 0));
            if (cw >= 40 && cw <= 400) data->settings->clockWidth = cw;

            int spacing = static_cast<int>(
                SendMessageW(GetDlgItem(hClk, IDC_SPIN_LINESPACING), UDM_GETPOS32, 0, 0));
            if (spacing >= 0 && spacing <= 20) data->settings->clockLineSpacing = spacing;

            int timeSz = static_cast<int>(
                SendMessageW(GetDlgItem(hClk, IDC_SPIN_TIMEFONTSIZE), UDM_GETPOS32, 0, 0));
            int dateSz = static_cast<int>(
                SendMessageW(GetDlgItem(hClk, IDC_SPIN_DATEFONTSIZE), UDM_GETPOS32, 0, 0));
            if (timeSz >= 6 && timeSz <= 36) data->settings->clockTimeFontSize = timeSz;
            if (dateSz >= 6 && dateSz <= 36) data->settings->clockDateFontSize = dateSz;
            // clockTimeColor and clockDateColor are updated immediately on pick

            // App Menu tab — controls are reparented into hScrollHosts[4]
            {
                HWND hLayout = GetDlgItem(hAm, IDC_COMBO_APPMENU_LAYOUT);
                int layoutIdx = static_cast<int>(SendMessageW(hLayout, CB_GETCURSEL, 0, 0));
                if (layoutIdx >= 0)
                    data->settings->appMenuLayout = static_cast<AppMenuLayout>(layoutIdx);

                auto readSpin = [&](int spinId, int /*lo*/, int /*hi*/) -> int {
                    return static_cast<int>(
                        SendMessageW(GetDlgItem(hAm, spinId), UDM_GETPOS32, 0, 0));
                };

                int amW  = readSpin(IDC_SPIN_APPMENU_WIDTH, 120, 800);
                int amMH = readSpin(IDC_SPIN_APPMENU_MAXHEIGHT, 100, 2000);
                int amEH = readSpin(IDC_SPIN_APPMENU_ENTRYH, 20, 80);
                int amGC = readSpin(IDC_SPIN_APPMENU_GRIDCOLS, 1, 12);
                int amGR = readSpin(IDC_SPIN_APPMENU_GRIDROWS, 1, 20);
                int amLF = readSpin(IDC_SPIN_APPMENU_LISTFS, 6, 36);
                int amGF = readSpin(IDC_SPIN_APPMENU_GRIDFS, 6, 36);
                int amMgn = readSpin(IDC_SPIN_APPMENU_MARGIN, 0, 40);
                int amPad = readSpin(IDC_SPIN_APPMENU_PADDING, 0, 40);

                if (amW  >= 120  && amW  <= 800)  data->settings->appMenuWidth        = amW;
                if (amMH >= 100  && amMH <= 2000) data->settings->appMenuMaxHeight    = amMH;
                if (amEH >= 20   && amEH <= 80)   data->settings->appMenuEntryHeight  = amEH;
                if (amGC >= 1    && amGC <= 12)   data->settings->appMenuGridCols     = amGC;
                if (amGR >= 1    && amGR <= 20)   data->settings->appMenuGridRows     = amGR;
                if (amLF >= 6    && amLF <= 36)   data->settings->appMenuListFontSize = amLF;
                if (amGF >= 6    && amGF <= 36)   data->settings->appMenuGridFontSize = amGF;
                if (amMgn >= 0   && amMgn <= 40)  data->settings->appMenuMargin       = amMgn;
                if (amPad >= 0   && amPad <= 40)  data->settings->appMenuPadding      = amPad;

                data->settings->appMenuSidebarEnabled =
                    IsDlgButtonChecked(hAm, IDC_CHECK_APPMENU_SIDEBAR) == BST_CHECKED;
                int amSW = readSpin(IDC_SPIN_APPMENU_SIDEBARW, 4, 120);
                if (amSW >= 4 && amSW <= 120) data->settings->appMenuSidebarWidth = amSW;
                data->settings->appMenuSidebarShowExplorer =
                    IsDlgButtonChecked(hAm, IDC_CHECK_APPMENU_SIDEBAR_EXPLORER) == BST_CHECKED;
                data->settings->appMenuSidebarShowSettings =
                    IsDlgButtonChecked(hAm, IDC_CHECK_APPMENU_SIDEBAR_SETTINGS) == BST_CHECKED;
                data->settings->appMenuSidebarShowPower =
                    IsDlgButtonChecked(hAm, IDC_CHECK_APPMENU_SIDEBAR_POWER) == BST_CHECKED;

                bool flatSubmenus = IsDlgButtonChecked(hAm, IDC_CHECK_APPMENU_FLATTEN_SUBMENUS) == BST_CHECKED;
                bool flatAll      = IsDlgButtonChecked(hAm, IDC_CHECK_APPMENU_FLATTEN_ALL)      == BST_CHECKED;
                data->settings->appMenuFlattenMode =
                    flatAll      ? AppMenuFlattenMode::All     :
                    flatSubmenus ? AppMenuFlattenMode::Submenus :
                                   AppMenuFlattenMode::None;

                data->settings->appMenuSearchEnabled =
                    IsDlgButtonChecked(hAm, IDC_CHECK_APPMENU_SEARCH) == BST_CHECKED;
                data->settings->appMenuSearchFuzzy =
                    IsDlgButtonChecked(hAm, IDC_CHECK_APPMENU_SEARCH_FUZZY) == BST_CHECKED;
                data->settings->appMenuSearchSystem =
                    IsDlgButtonChecked(hAm, IDC_CHECK_APPMENU_SEARCH_SYSTEM) == BST_CHECKED;

                int amCPW = readSpin(IDC_SPIN_APPMENU_CLASSIC_PW, 80, 400);
                if (amCPW >= 80 && amCPW <= 400) data->settings->appMenuClassicPanelWidth = amCPW;
                data->settings->appMenuClassicShowDocuments   = IsDlgButtonChecked(hAm, IDC_CHECK_APPMENU_CLASSIC_DOCS)     == BST_CHECKED;
                data->settings->appMenuClassicShowPictures    = IsDlgButtonChecked(hAm, IDC_CHECK_APPMENU_CLASSIC_PICS)     == BST_CHECKED;
                data->settings->appMenuClassicShowMusic       = IsDlgButtonChecked(hAm, IDC_CHECK_APPMENU_CLASSIC_MUSIC)    == BST_CHECKED;
                data->settings->appMenuClassicShowDownloads   = IsDlgButtonChecked(hAm, IDC_CHECK_APPMENU_CLASSIC_DLOADS)   == BST_CHECKED;
                data->settings->appMenuClassicShowRecentItems = IsDlgButtonChecked(hAm, IDC_CHECK_APPMENU_CLASSIC_RECENT)   == BST_CHECKED;
                data->settings->appMenuClassicShowThisPC      = IsDlgButtonChecked(hAm, IDC_CHECK_APPMENU_CLASSIC_THISPC)   == BST_CHECKED;
                data->settings->appMenuClassicShowControlPanel= IsDlgButtonChecked(hAm, IDC_CHECK_APPMENU_CLASSIC_CTRL)     == BST_CHECKED;
                data->settings->appMenuClassicShowWinSettings = IsDlgButtonChecked(hAm, IDC_CHECK_APPMENU_CLASSIC_WINSETT)  == BST_CHECKED;
                data->settings->appMenuClassicShowRun         = IsDlgButtonChecked(hAm, IDC_CHECK_APPMENU_CLASSIC_RUN)      == BST_CHECKED;
                data->settings->appMenuClassicShowShutDown    = IsDlgButtonChecked(hAm, IDC_CHECK_APPMENU_CLASSIC_SHUTDOWN) == BST_CHECKED;
                data->settings->appMenuPinnedPerMonitor       = IsDlgButtonChecked(hAm, IDC_CHECK_APPMENU_PINNED_PER_MONITOR) == BST_CHECKED;
            }

            EndDialog(hwnd, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    default: break;
    }
    return FALSE;
}

bool SettingsDialog::Show(HWND hwndParent, Settings& settings)
{
    Settings localCopy = settings;
    DlgData data{ &localCopy };

    auto prevCtx = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    INT_PTR result = DialogBoxParamW(
        GetModuleHandleW(nullptr),
        MAKEINTRESOURCE(IDD_SETTINGS),
        hwndParent,
        SettingsDialog::DlgProc,
        reinterpret_cast<LPARAM>(&data));

    SetThreadDpiAwarenessContext(prevCtx);

    if (result == IDOK)
        settings = localCopy;

    return result == IDOK;
}
