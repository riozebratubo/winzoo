#include "PopupMenu.h"
#include <windowsx.h>
#include <algorithm>
#include <utility>

static constexpr wchar_t kClassName[] = L"WinzooPopupMenu";

bool PopupMenu::RegisterWndClass(HINSTANCE hInst)
{
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    wc.lpfnWndProc   = PopupMenu::WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;

    WNDCLASSEXW existing = {};
    existing.cbSize = sizeof(existing);
    if (GetClassInfoExW(hInst, kClassName, &existing))
        return true;

    return RegisterClassExW(&wc) != 0;
}

LRESULT CALLBACK PopupMenu::WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    PopupMenu* pThis = nullptr;

    if (uMsg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        pThis = static_cast<PopupMenu*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        pThis->hwnd_ = hwnd;
    } else {
        pThis = reinterpret_cast<PopupMenu*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (pThis)
        return pThis->HandleMessage(hwnd, uMsg, wParam, lParam);
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

LRESULT PopupMenu::HandleMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        Paint(hdc, rc.right, rc.bottom);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int idx = HitTestItem(pt);
        if (idx >= 0 && (items_[idx].isSeparator || items_[idx].isHeader || items_[idx].isDisabled))
            idx = -1;
        if (idx != hovered_) {
            hovered_ = idx;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int idx = HitTestItem(pt);
        if (idx >= 0 && !items_[idx].isSeparator && !items_[idx].isHeader && !items_[idx].isDisabled) {
            result_ = items_[idx].id;
        }
        done_ = true;
        DestroyWindow(hwnd);
        return 0;
    }

    case WM_KEYDOWN:
        switch (wParam) {
        case VK_UP:
            for (int i = hovered_ - 1; i >= 0; --i) {
                if (!items_[i].isSeparator && !items_[i].isHeader && !items_[i].isDisabled) { hovered_ = i; break; }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        case VK_DOWN:
            for (int i = hovered_ + 1; std::cmp_less(i, items_.size()); ++i) {
                if (!items_[i].isSeparator && !items_[i].isHeader && !items_[i].isDisabled) { hovered_ = i; break; }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        case VK_RETURN:
            if (hovered_ >= 0 && !items_[hovered_].isSeparator && !items_[hovered_].isHeader && !items_[hovered_].isDisabled)
                result_ = items_[hovered_].id;
            done_ = true;
            DestroyWindow(hwnd);
            break;
        case VK_ESCAPE:
            done_ = true;
            DestroyWindow(hwnd);
            break;
        default: break;
        }
        return 0;

    case WM_KILLFOCUS:
        done_ = true;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        done_ = true;
        return 0;
    default: break;
    }

    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

void PopupMenu::Paint(HDC hdc, int w, int h)
{
    RECT bg = { 0, 0, w, h };
    HBRUSH bgBrush = CreateSolidBrush(colors_.menuBg);
    FillRect(hdc, &bg, bgBrush);
    DeleteObject(bgBrush);

    LOGFONTW lf = {};
    lf.lfHeight  = -MulDiv(9, dpi_, 72);
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    HFONT font    = CreateFontIndirectW(&lf);

    LOGFONTW lfHeader = lf;
    lfHeader.lfWeight = FW_SEMIBOLD;
    lfHeader.lfHeight = -MulDiv(8, dpi_, 72);
    HFONT headerFont = CreateFontIndirectW(&lfHeader);

    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, font));

    int itemH = ItemHeight();
    int y = 0;
    int pad = Scale(8, dpi_);

    for (int i = 0; std::cmp_less(i, items_.size()); ++i) {
        const auto& item = items_[i];

        if (item.isSeparator) {
            int mid = y + itemH / 2;
            HPEN pen = CreatePen(PS_SOLID, 1, colors_.separator);
            HPEN old = static_cast<HPEN>(SelectObject(hdc, pen));
            MoveToEx(hdc, pad, mid, nullptr);
            LineTo(hdc, w - pad, mid);
            SelectObject(hdc, old);
            DeleteObject(pen);
        } else if (item.isHeader) {
            SelectObject(hdc, headerFont);
            RECT textRect = { pad, y, w - pad, y + itemH };
            SetTextColor(hdc, colors_.textDimmed);
            SetBkMode(hdc, TRANSPARENT);
            DrawTextW(hdc, item.label.c_str(), -1, &textRect,
                      DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
            SelectObject(hdc, font);
        } else {
            if (i == hovered_) {
                RECT hr = { 0, y, w, y + itemH };
                HBRUSH hb = CreateSolidBrush(colors_.menuHover);
                FillRect(hdc, &hr, hb);
                DeleteObject(hb);
            }

            COLORREF textCol = item.isDisabled ? colors_.textDimmed : colors_.menuText;

            if (item.isChecked) {
                RECT checkRect = { pad, y, pad + itemH, y + itemH };
                SetTextColor(hdc, textCol);
                SetBkMode(hdc, TRANSPARENT);
                DrawTextW(hdc, L"✓", 1, &checkRect,
                          DT_SINGLELINE | DT_VCENTER | DT_CENTER);
            }

            RECT textRect = { pad + itemH, y, w - pad, y + itemH };
            SetTextColor(hdc, textCol);
            SetBkMode(hdc, TRANSPARENT);
            DrawTextW(hdc, item.label.c_str(), -1, &textRect,
                      DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
        y += itemH;
    }

    SelectObject(hdc, oldFont);
    DeleteObject(font);
    DeleteObject(headerFont);
}

int PopupMenu::HitTestItem(POINT ptClient) const
{
    int itemH = ItemHeight();
    int idx   = ptClient.y / itemH;
    if (idx < 0 || std::cmp_greater_equal(idx, items_.size())) return -1;
    return idx;
}

UINT PopupMenu::Show(HWND hwndOwner, POINT ptScreen,
                     std::vector<MenuItem> items,
                     const ThemeColors& colors, int dpi)
{
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    if (!RegisterWndClass(hInst)) return 0;

    PopupMenu menu;
    menu.items_  = std::move(items);
    menu.colors_ = colors;
    menu.dpi_    = dpi;

    int itemH = menu.ItemHeight();
    int menuH = static_cast<int>(menu.items_.size()) * itemH;

    // Calculate menu width based on item text
    int menuW = Scale(200, dpi);
    {
        HDC hdc = GetDC(hwndOwner);
        LOGFONTW lf = {};
        lf.lfHeight  = -MulDiv(9, dpi, 72);
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
        HFONT font = CreateFontIndirectW(&lf);
        HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, font));
        int pad = Scale(8, dpi);
        for (const auto& item : menu.items_) {
            if (item.isSeparator || item.label.empty()) continue;
            SIZE sz = {};
            GetTextExtentPoint32W(hdc, item.label.c_str(),
                                  static_cast<int>(item.label.size()), &sz);
            int needed = sz.cx + pad * 2 + itemH + pad;
            if (needed > menuW) menuW = needed;
        }
        SelectObject(hdc, oldFont);
        DeleteObject(font);
        ReleaseDC(hwndOwner, hdc);
        int maxW = Scale(360, dpi);
        if (menuW > maxW) menuW = maxW;
    }

    // Clamp to the work area of the monitor the point is on
    HMONITOR hMon = MonitorFromPoint(ptScreen, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfo(hMon, &mi);
    RECT workArea = mi.rcWork;
    POINT pt = ptScreen;
    if (pt.x + menuW > workArea.right)  pt.x = workArea.right  - menuW;
    if (pt.y + menuH > workArea.bottom) pt.y = workArea.bottom - menuH;
    if (pt.x < workArea.left) pt.x = workArea.left;
    if (pt.y < workArea.top)  pt.y = workArea.top;

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST,
        kClassName, nullptr,
        WS_POPUP | WS_BORDER,
        pt.x, pt.y, menuW, menuH,
        hwndOwner, nullptr, hInst, &menu);

    if (!hwnd) return 0;

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetForegroundWindow(hwnd);

    MSG msg;
    while (!menu.done_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (IsWindow(hwnd)) DestroyWindow(hwnd);

    return menu.result_;
}
