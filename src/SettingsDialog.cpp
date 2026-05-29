#include "SettingsDialog.h"
#include <commctrl.h>
#include <uxtheme.h>
#include "resource.h"

static constexpr const wchar_t* kPositions[] = {
    L"Top", L"Bottom", L"Left", L"Right", L"Floating"
};

static constexpr const wchar_t* kThemes[] = {
    L"Dark", L"Light", L"Accent (Blue)", L"Forest (Green)", L"Sunset (Orange)"
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
    IDC_LBL_TIMEFMT, IDC_EDIT_TIMEFMT,
    IDC_LBL_DATEFMT, IDC_EDIT_DATEFMT,
    IDC_LBL_CLOCKW,  IDC_EDIT_CLOCKW, IDC_SPIN_CLOCKW,
    IDC_LBL_TOKENS,
    0
};

static const int* kTabGroups[] = { kGeneralControls, kAppBtnControls, kClockControls };

static void ShowTab(HWND hwnd, int tab)
{
    for (int g = 0; g < 3; ++g) {
        int cmd = (g == tab) ? SW_SHOW : SW_HIDE;
        for (const int* id = kTabGroups[g]; *id; ++id)
            ShowWindow(GetDlgItem(hwnd, *id), cmd);
    }
}

static void SetClockControlsEnabled(HWND hwnd, bool enabled)
{
    static const int kIds[] = {
        IDC_LBL_TIMEFMT, IDC_EDIT_TIMEFMT,
        IDC_LBL_DATEFMT, IDC_EDIT_DATEFMT,
        IDC_LBL_CLOCKW,  IDC_EDIT_CLOCKW, IDC_SPIN_CLOCKW,
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

        SetClockControlsEnabled(hwnd, data->settings->showClock);

        // Tab control
        HWND hTab = GetDlgItem(hwnd, IDC_TAB_SETTINGS);
        TCITEMW tci = {};
        tci.mask = TCIF_TEXT;
        wchar_t t0[] = L"General", t1[] = L"App Buttons", t2[] = L"System Clock";
        tci.pszText = t0; TabCtrl_InsertItem(hTab, 0, &tci);
        tci.pszText = t1; TabCtrl_InsertItem(hTab, 1, &tci);
        tci.pszText = t2; TabCtrl_InsertItem(hTab, 2, &tci);

        ShowTab(hwnd, 0);

        // Disable visual styles on checkboxes so they respect the transparent
        // background brush from WM_CTLCOLORBTN instead of painting their own.
        static const int kCheckIds[] = {
            IDC_CHECK_MIDDLECLICK, IDC_CHECK_RIGHTCLICKGAP, IDC_CHECK_SHOWCLOCK, 0
        };
        for (const int* id = kCheckIds; *id; ++id)
            SetWindowTheme(GetDlgItem(hwnd, *id), L"", L"");

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

    case WM_COMMAND:
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
