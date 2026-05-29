#include "SettingsDialog.h"
#include <commctrl.h>
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

INT_PTR CALLBACK SettingsDialog::DlgProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    DlgData* data = reinterpret_cast<DlgData*>(GetWindowLongPtrW(hwnd, DWLP_USER));

    switch (uMsg) {
    case WM_INITDIALOG: {
        data = reinterpret_cast<DlgData*>(lParam);
        SetWindowLongPtrW(hwnd, DWLP_USER, lParam);

        HWND hPos   = GetDlgItem(hwnd, IDC_COMBO_POSITION);
        HWND hTheme = GetDlgItem(hwnd, IDC_COMBO_THEME);

        for (auto* s : kPositions) SendMessageW(hPos,   CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
        for (auto* s : kThemes)    SendMessageW(hTheme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));

        SendMessageW(hPos,   CB_SETCURSEL, static_cast<WPARAM>(data->settings->position), 0);
        SendMessageW(hTheme, CB_SETCURSEL, static_cast<WPARAM>(data->settings->theme),    0);

        HWND hSpin = GetDlgItem(hwnd, IDC_SPIN_THICKNESS);
        HWND hEdit = GetDlgItem(hwnd, IDC_EDIT_THICKNESS);
        SendMessageW(hSpin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(hEdit), 0);
        SendMessageW(hSpin, UDM_SETRANGE32, 28, 120);
        SendMessageW(hSpin, UDM_SETPOS32, 0, static_cast<LPARAM>(data->settings->thickness));

        CheckDlgButton(hwnd, IDC_CHECK_MIDDLECLICK,
                       data->settings->middleClickClose ? BST_CHECKED : BST_UNCHECKED);

        return TRUE;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK && data) {
            HWND hPos   = GetDlgItem(hwnd, IDC_COMBO_POSITION);
            HWND hTheme = GetDlgItem(hwnd, IDC_COMBO_THEME);
            HWND hSpin  = GetDlgItem(hwnd, IDC_SPIN_THICKNESS);

            int posIdx   = static_cast<int>(SendMessageW(hPos,   CB_GETCURSEL, 0, 0));
            int themeIdx = static_cast<int>(SendMessageW(hTheme, CB_GETCURSEL, 0, 0));
            int thick    = static_cast<int>(SendMessageW(hSpin,  UDM_GETPOS32, 0, 0));

            if (posIdx >= 0)   data->settings->position  = static_cast<TaskbarPosition>(posIdx);
            if (themeIdx >= 0) data->settings->theme      = static_cast<ThemePreset>(themeIdx);
            if (thick >= 28 && thick <= 120) data->settings->thickness = thick;
            data->settings->middleClickClose =
                IsDlgButtonChecked(hwnd, IDC_CHECK_MIDDLECLICK) == BST_CHECKED;

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
