#include "InputDialog.h"
#include "resource.h"

namespace {
struct InputCtx {
    const wchar_t* title;
    const wchar_t* prompt;
    std::wstring*  inOut;
};

std::wstring Trim(const std::wstring& s)
{
    size_t b = s.find_first_not_of(L" \t\r\n");
    if (b == std::wstring::npos) return L"";
    size_t e = s.find_last_not_of(L" \t\r\n");
    return s.substr(b, e - b + 1);
}
} // namespace

INT_PTR CALLBACK InputDialog::DlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_INITDIALOG: {
        auto* ctx = reinterpret_cast<InputCtx*>(lParam);
        SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(ctx));
        if (ctx) {
            if (ctx->title)  SetWindowTextW(hwnd, ctx->title);
            if (ctx->prompt) SetDlgItemTextW(hwnd, IDC_INPUT_PROMPT, ctx->prompt);
            if (ctx->inOut)  SetDlgItemTextW(hwnd, IDC_INPUT_EDIT, ctx->inOut->c_str());
        }
        HWND edit = GetDlgItem(hwnd, IDC_INPUT_EDIT);
        SetFocus(edit);
        SendMessageW(edit, EM_SETSEL, 0, -1);  // select existing text
        return FALSE;  // we set focus ourselves
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK: {
            auto* ctx = reinterpret_cast<InputCtx*>(GetWindowLongPtrW(hwnd, DWLP_USER));
            if (ctx && ctx->inOut) {
                int len = GetWindowTextLengthW(GetDlgItem(hwnd, IDC_INPUT_EDIT));
                std::wstring buf(static_cast<size_t>(len) + 1, L'\0');
                GetDlgItemTextW(hwnd, IDC_INPUT_EDIT, buf.data(), len + 1);
                buf.resize(static_cast<size_t>(len));
                *ctx->inOut = Trim(buf);
            }
            EndDialog(hwnd, 1);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hwnd, 0);
            return TRUE;
        }
        return FALSE;

    case WM_CLOSE:
        EndDialog(hwnd, 0);
        return TRUE;
    }
    return FALSE;
}

bool InputDialog::Show(HWND parent, const wchar_t* title, const wchar_t* prompt,
                       std::wstring& inOut)
{
    InputCtx ctx{ title, prompt, &inOut };
    INT_PTR r = DialogBoxParamW(GetModuleHandleW(nullptr),
                                MAKEINTRESOURCEW(IDD_INPUT), parent,
                                &InputDialog::DlgProc,
                                reinterpret_cast<LPARAM>(&ctx));
    return r == 1;
}
