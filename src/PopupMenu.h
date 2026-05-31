#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "Theme.h"
#include "Dpi.h"

struct MenuItem {
    std::wstring label;
    UINT         id          = 0;
    bool         isSeparator = false;
    bool         isChecked   = false;
    bool         isDisabled  = false;
    bool         isHeader    = false;   // non-clickable section header
};

class PopupMenu {
public:
    // Shows a custom-drawn popup menu and returns the selected item ID, or 0.
    static UINT Show(HWND hwndOwner, POINT ptScreen,
                     std::vector<MenuItem> items,
                     const ThemeColors& colors, int dpi);

private:
    static bool         RegisterWndClass(HINSTANCE hInst);
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    LRESULT HandleMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    void    Paint(HDC hdc, int w, int h);
    int     HitTestItem(POINT ptClient) const;
    int     ItemHeight() const { return Scale(24, dpi_); }

    std::vector<MenuItem> items_;
    ThemeColors           colors_ = {};
    int                   dpi_    = 96;
    int                   hovered_ = -1;
    UINT                  result_  = 0;
    bool                  done_    = false;
    HWND                  hwnd_    = nullptr;
};
