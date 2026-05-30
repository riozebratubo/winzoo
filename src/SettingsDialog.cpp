#include "SettingsDialog.h"
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
};

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
    0
};

static const int* kTabGroups[] = { kGeneralControls, kAppBtnControls, kClockControls, kAppMenuControls };

static void ShowTab(HWND hwnd, int tab)
{
    for (int g = 0; g < 4; ++g) {
        int cmd = (g == tab) ? SW_SHOW : SW_HIDE;
        for (const int* id = kTabGroups[g]; *id; ++id)
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
            IDC_CHECK_MIDDLECLICK, IDC_CHECK_RIGHTCLICKGAP, IDC_CHECK_SHOWCLOCK, 0
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
        }

        // ShowTab must run last so that UDM_SETBUDDY calls (which make edit buddies
        // visible) are all done before we hide controls belonging to inactive tabs.
        ShowTab(hwnd, 0);

        return TRUE;
    }

    case WM_NOTIFY: {
        auto* nm = reinterpret_cast<NMHDR*>(lParam);
        if (data && nm->idFrom == IDC_TAB_SETTINGS && nm->code == TCN_SELCHANGE) {
            int tab = TabCtrl_GetCurSel(GetDlgItem(hwnd, IDC_TAB_SETTINGS));
            ShowTab(hwnd, tab);
            if (tab == 2) {
                bool on = IsDlgButtonChecked(hwnd, IDC_CHECK_SHOWCLOCK) == BST_CHECKED;
                SetClockControlsEnabled(hwnd, on);
            }
            return TRUE;
        }
        break;
    }

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

            // App Menu tab
            {
                HWND hLayout = GetDlgItem(hwnd, IDC_COMBO_APPMENU_LAYOUT);
                int layoutIdx = static_cast<int>(SendMessageW(hLayout, CB_GETCURSEL, 0, 0));
                if (layoutIdx >= 0)
                    data->settings->appMenuLayout = static_cast<AppMenuLayout>(layoutIdx);

                auto readSpin = [&](int spinId, int /*lo*/, int /*hi*/) -> int {
                    return static_cast<int>(
                        SendMessageW(GetDlgItem(hwnd, spinId), UDM_GETPOS32, 0, 0));
                };

                int amW  = readSpin(IDC_SPIN_APPMENU_WIDTH, 120, 800);
                int amMH = readSpin(IDC_SPIN_APPMENU_MAXHEIGHT, 100, 2000);
                int amEH = readSpin(IDC_SPIN_APPMENU_ENTRYH, 20, 80);
                int amGC = readSpin(IDC_SPIN_APPMENU_GRIDCOLS, 1, 12);
                int amGR = readSpin(IDC_SPIN_APPMENU_GRIDROWS, 1, 20);
                int amLF = readSpin(IDC_SPIN_APPMENU_LISTFS, 6, 36);
                int amGF = readSpin(IDC_SPIN_APPMENU_GRIDFS, 6, 36);

                if (amW  >= 120  && amW  <= 800)  data->settings->appMenuWidth        = amW;
                if (amMH >= 100  && amMH <= 2000) data->settings->appMenuMaxHeight    = amMH;
                if (amEH >= 20   && amEH <= 80)   data->settings->appMenuEntryHeight  = amEH;
                if (amGC >= 1    && amGC <= 12)   data->settings->appMenuGridCols     = amGC;
                if (amGR >= 1    && amGR <= 20)   data->settings->appMenuGridRows     = amGR;
                if (amLF >= 6    && amLF <= 36)   data->settings->appMenuListFontSize = amLF;
                if (amGF >= 6    && amGF <= 36)   data->settings->appMenuGridFontSize = amGF;
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
