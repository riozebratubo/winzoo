#include "SettingsDialog.h"
#include <algorithm>
#include <commctrl.h>
#include <commdlg.h>
#include <uxtheme.h>
#include "resource.h"

static constexpr const wchar_t* kPositions[] = {
    L"Top", L"Bottom", L"Left", L"Right", L"Floating"
};

static constexpr const wchar_t* kThemes[] = {
    L"Dark", L"Light", L"Accent (Blue)", L"Forest (Green)", L"Sunset (Orange)"
};

static constexpr const wchar_t* kAppMenuLayouts[] = {
    L"List", L"Grid"
};

struct DlgData {
    Settings* settings;
    HWND      hScrollHost = nullptr;
};

// Scroll host for the App Menu tab — scrolls its children vertically
static constexpr wchar_t kScrollHostClass[] = L"WinzooAppMenuScrollHost";

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
        return SendMessageW(GetParent(hwnd), uMsg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

static void RegisterScrollHostClass()
{
    WNDCLASSEXW existing = { sizeof(existing) };
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    if (GetClassInfoExW(hInst, kScrollHostClass, &existing)) return;
    WNDCLASSEXW wc    = { sizeof(wc) };
    wc.lpfnWndProc    = AppMenuScrollHostProc;
    wc.hInstance      = hInst;
    wc.hCursor        = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground  = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName  = kScrollHostClass;
    RegisterClassExW(&wc);
}

// Null-terminated control ID lists per tab
static const int kGeneralControls[] = {
    IDC_LBL_POSITION,  IDC_COMBO_POSITION,
    IDC_LBL_THEME,     IDC_COMBO_THEME,
    IDC_LBL_THICKNESS, IDC_EDIT_THICKNESS, IDC_SPIN_THICKNESS,
    0
};
static const int kAppBtnControls[] = {
    IDC_LBL_MAXBTNW,        IDC_EDIT_MAXBTNW, IDC_SPIN_MAXBTNW,
    IDC_LBL_MINBTNW,        IDC_EDIT_MINBTNW, IDC_SPIN_MINBTNW,
    IDC_CHECK_MIDDLECLICK,  IDC_CHECK_RIGHTCLICKGAP,
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
    0
};

static const int* kTabGroups[] = { kGeneralControls, kAppBtnControls, kClockControls, kAppMenuControls };

static void ShowTab(HWND hwnd, int tab, DlgData* data)
{
    // Tabs 0–2 are managed directly; tab 3 (App Menu) uses the scroll host
    for (int g = 0; g < 3; ++g) {
        int cmd = (g == tab) ? SW_SHOW : SW_HIDE;
        for (const int* id = kTabGroups[g]; *id; ++id)
            ShowWindow(GetDlgItem(hwnd, *id), cmd);
    }
    if (data && data->hScrollHost) {
        ShowWindow(data->hScrollHost, tab == 3 ? SW_SHOW : SW_HIDE);
    } else {
        // Fallback: scroll host not yet created
        int cmd = (tab == 3) ? SW_SHOW : SW_HIDE;
        for (const int* id = kAppMenuControls; *id; ++id)
            ShowWindow(GetDlgItem(hwnd, *id), cmd);
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

INT_PTR CALLBACK SettingsDialog::DlgProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    DlgData* data = reinterpret_cast<DlgData*>(GetWindowLongPtrW(hwnd, DWLP_USER));

    switch (uMsg) {
    case WM_INITDIALOG: {
        data = reinterpret_cast<DlgData*>(lParam);
        SetWindowLongPtrW(hwnd, DWLP_USER, lParam);

        // General
        HWND hPos   = GetDlgItem(hwnd, IDC_COMBO_POSITION);
        HWND hTheme = GetDlgItem(hwnd, IDC_COMBO_THEME);
        for (auto* s : kPositions) SendMessageW(hPos,   CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
        for (auto* s : kThemes)    SendMessageW(hTheme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
        SendMessageW(hPos,   CB_SETCURSEL, static_cast<WPARAM>(data->settings->position), 0);
        SendMessageW(hTheme, CB_SETCURSEL, static_cast<WPARAM>(data->settings->theme),    0);

        HWND hThickSpin = GetDlgItem(hwnd, IDC_SPIN_THICKNESS);
        HWND hThickEdit = GetDlgItem(hwnd, IDC_EDIT_THICKNESS);
        SendMessageW(hThickSpin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(hThickEdit), 0);
        SendMessageW(hThickSpin, UDM_SETRANGE32, 28, 120);
        SendMessageW(hThickSpin, UDM_SETPOS32, 0, static_cast<LPARAM>(data->settings->thickness));

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

        CheckDlgButton(hwnd, IDC_CHECK_MIDDLECLICK,
                       data->settings->middleClickClose ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_CHECK_RIGHTCLICKGAP,
                       data->settings->showRightClickGap ? BST_CHECKED : BST_UNCHECKED);

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
        wchar_t t0[] = L"General", t1[] = L"App Buttons", t2[] = L"System Clock", t3[] = L"App Menu";
        tci.pszText = t0; TabCtrl_InsertItem(hTab, 0, &tci);
        tci.pszText = t1; TabCtrl_InsertItem(hTab, 1, &tci);
        tci.pszText = t2; TabCtrl_InsertItem(hTab, 2, &tci);
        tci.pszText = t3; TabCtrl_InsertItem(hTab, 3, &tci);

        // Disable visual styles on checkboxes so they respect the transparent
        // background brush from WM_CTLCOLORBTN instead of painting their own.
        static const int kCheckIds[] = {
            IDC_CHECK_MIDDLECLICK, IDC_CHECK_RIGHTCLICKGAP, IDC_CHECK_SHOWCLOCK,
            IDC_CHECK_APPMENU_SIDEBAR,
            IDC_CHECK_APPMENU_SIDEBAR_EXPLORER,
            IDC_CHECK_APPMENU_SIDEBAR_SETTINGS,
            IDC_CHECK_APPMENU_SIDEBAR_POWER,
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
        }

        // Build scrollable host for the App Menu tab
        {
            RegisterScrollHostClass();

            // Compute the tab's content area in dialog client coordinates
            HWND hTabCtrl = GetDlgItem(hwnd, IDC_TAB_SETTINGS);
            RECT tabWndRect;
            GetWindowRect(hTabCtrl, &tabWndRect);
            RECT content = tabWndRect;
            TabCtrl_AdjustRect(hTabCtrl, FALSE, &content);
            MapWindowPoints(HWND_DESKTOP, hwnd, reinterpret_cast<LPPOINT>(&content), 2);

            int panelW = content.right  - content.left;
            int panelH = content.bottom - content.top;

            HWND hPanel = CreateWindowExW(
                WS_EX_CONTROLPARENT,
                kScrollHostClass, nullptr,
                WS_CHILD | WS_VSCROLL,
                content.left, content.top, panelW, panelH,
                hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);

            data->hScrollHost = hPanel;

            if (hPanel) {
                // Reparent every App Menu control into the scroll host and
                // convert its position to be relative to the scroll host.
                int contentH = 0;
                for (const int* id = kAppMenuControls; *id; ++id) {
                    HWND hCtrl = GetDlgItem(hwnd, *id);
                    if (!hCtrl) continue;
                    RECT r;
                    GetWindowRect(hCtrl, &r);
                    SetParent(hCtrl, hPanel);
                    MapWindowPoints(HWND_DESKTOP, hPanel,
                                    reinterpret_cast<LPPOINT>(&r), 2);
                    SetWindowPos(hCtrl, nullptr, r.left, r.top, 0, 0,
                                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                    contentH = std::max(contentH, static_cast<int>(r.bottom));
                }
                contentH += 4; // small bottom padding

                SCROLLINFO si = {};
                si.cbSize = sizeof(si);
                si.fMask  = SIF_ALL;
                si.nMin   = 0;
                si.nMax   = contentH;
                si.nPage  = static_cast<UINT>(panelH);
                si.nPos   = 0;
                SetScrollInfo(hPanel, SB_VERT, &si, TRUE);
            }
        }

        // ShowTab must run last so that UDM_SETBUDDY calls (which make edit buddies
        // visible) are all done before we hide controls belonging to inactive tabs.
        ShowTab(hwnd, 0, data);

        return TRUE;
    }

    case WM_NOTIFY: {
        auto* nm = reinterpret_cast<NMHDR*>(lParam);
        if (data && nm->idFrom == IDC_TAB_SETTINGS && nm->code == TCN_SELCHANGE) {
            int tab = TabCtrl_GetCurSel(GetDlgItem(hwnd, IDC_TAB_SETTINGS));
            ShowTab(hwnd, tab, data);
            if (tab == 2) {
                bool on = IsDlgButtonChecked(hwnd, IDC_CHECK_SHOWCLOCK) == BST_CHECKED;
                SetClockControlsEnabled(hwnd, on);
            }
            return TRUE;
        }
        break;
    }

    case WM_MOUSEWHEEL:
        if (data && data->hScrollHost && IsWindowVisible(data->hScrollHost)) {
            SendMessageW(data->hScrollHost, WM_MOUSEWHEEL, wParam, lParam);
            return TRUE;
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
        if (di->CtlID == IDC_BTN_TIMECOLOR || di->CtlID == IDC_BTN_DATECOLOR) {
            COLORREF color = (di->CtlID == IDC_BTN_TIMECOLOR)
                             ? data->settings->clockTimeColor
                             : data->settings->clockDateColor;
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
        if ((LOWORD(wParam) == IDC_BTN_TIMECOLOR || LOWORD(wParam) == IDC_BTN_DATECOLOR)
            && data)
        {
            COLORREF* colorField = (LOWORD(wParam) == IDC_BTN_TIMECOLOR)
                                   ? &data->settings->clockTimeColor
                                   : &data->settings->clockDateColor;
            static COLORREF customColors[16] = {};
            CHOOSECOLORW cc    = {};
            cc.lStructSize     = sizeof(cc);
            cc.hwndOwner       = hwnd;
            cc.rgbResult       = *colorField;
            cc.lpCustColors    = customColors;
            cc.Flags           = CC_RGBINIT | CC_FULLOPEN;
            if (ChooseColorW(&cc)) {
                *colorField = cc.rgbResult;
                InvalidateRect(GetDlgItem(hwnd, LOWORD(wParam)), nullptr, FALSE);
            }
            return TRUE;
        }
        if (LOWORD(wParam) == IDC_CHECK_SHOWCLOCK) {
            bool checked = IsDlgButtonChecked(hwnd, IDC_CHECK_SHOWCLOCK) == BST_CHECKED;
            SetClockControlsEnabled(hwnd, checked);
            return TRUE;
        }
        if (LOWORD(wParam) == IDC_CHECK_APPMENU_SIDEBAR) {
            HWND hAm = (data && data->hScrollHost) ? data->hScrollHost : hwnd;
            bool checked = IsDlgButtonChecked(hAm, IDC_CHECK_APPMENU_SIDEBAR) == BST_CHECKED;
            SetSidebarControlsEnabled(hAm, checked);
            return TRUE;
        }
        if (LOWORD(wParam) == IDOK && data) {
            HWND hPos   = GetDlgItem(hwnd, IDC_COMBO_POSITION);
            HWND hTheme = GetDlgItem(hwnd, IDC_COMBO_THEME);

            int posIdx   = static_cast<int>(SendMessageW(hPos,   CB_GETCURSEL, 0, 0));
            int themeIdx = static_cast<int>(SendMessageW(hTheme, CB_GETCURSEL, 0, 0));
            int thick    = static_cast<int>(
                SendMessageW(GetDlgItem(hwnd, IDC_SPIN_THICKNESS), UDM_GETPOS32, 0, 0));

            if (posIdx >= 0)                          data->settings->position  = static_cast<TaskbarPosition>(posIdx);
            if (themeIdx >= 0)                        data->settings->theme     = static_cast<ThemePreset>(themeIdx);
            if (thick >= 28 && thick <= 120)          data->settings->thickness = thick;

            int maxW = static_cast<int>(
                SendMessageW(GetDlgItem(hwnd, IDC_SPIN_MAXBTNW), UDM_GETPOS32, 0, 0));
            int minW = static_cast<int>(
                SendMessageW(GetDlgItem(hwnd, IDC_SPIN_MINBTNW), UDM_GETPOS32, 0, 0));
            if (maxW >= 48 && maxW <= 400) data->settings->maxButtonWidth = maxW;
            if (minW >= 24 && minW <= 400) data->settings->minButtonWidth = minW;
            if (data->settings->minButtonWidth > data->settings->maxButtonWidth)
                data->settings->minButtonWidth = data->settings->maxButtonWidth;

            data->settings->middleClickClose =
                IsDlgButtonChecked(hwnd, IDC_CHECK_MIDDLECLICK) == BST_CHECKED;
            data->settings->showRightClickGap =
                IsDlgButtonChecked(hwnd, IDC_CHECK_RIGHTCLICKGAP) == BST_CHECKED;

            data->settings->showClock =
                IsDlgButtonChecked(hwnd, IDC_CHECK_SHOWCLOCK) == BST_CHECKED;

            wchar_t buf[128];
            GetDlgItemTextW(hwnd, IDC_EDIT_TIMEFMT, buf, 128);
            data->settings->clockTimeFormat = buf;
            GetDlgItemTextW(hwnd, IDC_EDIT_DATEFMT, buf, 128);
            data->settings->clockDateFormat = buf;

            int cw = static_cast<int>(
                SendMessageW(GetDlgItem(hwnd, IDC_SPIN_CLOCKW), UDM_GETPOS32, 0, 0));
            if (cw >= 40 && cw <= 400) data->settings->clockWidth = cw;

            int spacing = static_cast<int>(
                SendMessageW(GetDlgItem(hwnd, IDC_SPIN_LINESPACING), UDM_GETPOS32, 0, 0));
            if (spacing >= 0 && spacing <= 20) data->settings->clockLineSpacing = spacing;

            int timeSz = static_cast<int>(
                SendMessageW(GetDlgItem(hwnd, IDC_SPIN_TIMEFONTSIZE), UDM_GETPOS32, 0, 0));
            int dateSz = static_cast<int>(
                SendMessageW(GetDlgItem(hwnd, IDC_SPIN_DATEFONTSIZE), UDM_GETPOS32, 0, 0));
            if (timeSz >= 6 && timeSz <= 36) data->settings->clockTimeFontSize = timeSz;
            if (dateSz >= 6 && dateSz <= 36) data->settings->clockDateFontSize = dateSz;
            // clockTimeColor and clockDateColor are updated immediately on pick

            // App Menu tab — controls are reparented into hScrollHost, so look up from there
            {
                HWND hAm = data->hScrollHost ? data->hScrollHost : hwnd;
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
            }

            EndDialog(hwnd, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

bool SettingsDialog::Show(HWND hwndParent, Settings& settings)
{
    DlgData data{ &settings };

    auto prevCtx = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    INT_PTR result = DialogBoxParamW(
        GetModuleHandleW(nullptr),
        MAKEINTRESOURCE(IDD_SETTINGS),
        hwndParent,
        SettingsDialog::DlgProc,
        reinterpret_cast<LPARAM>(&data));

    SetThreadDpiAwarenessContext(prevCtx);

    return result == IDOK;
}
