#pragma once
#include <windows.h>
#include <string>

// Minimal modal text-input dialog (a prompt label, a single edit box, OK/Cancel).
// Uses the IDD_INPUT resource. Returns true if the user pressed OK, with the
// entered text in `inOut` (whitespace-trimmed); false on Cancel/close.
class InputDialog {
public:
    static bool Show(HWND parent, const wchar_t* title, const wchar_t* prompt,
                     std::wstring& inOut);

private:
    static INT_PTR CALLBACK DlgProc(HWND, UINT, WPARAM, LPARAM);
};
