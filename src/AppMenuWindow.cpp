#include "AppMenuWindow.h"
#include "Dpi.h"
#include <windowsx.h>
#include <shellapi.h>
#include <algorithm>

static constexpr wchar_t kAppMenuClass[] = L"WinzooAppMenu";

bool AppMenuWindow::RegisterWndClass(HINSTANCE hInst)
{
    WNDCLASSEXW existing = { sizeof(existing) };
    if (GetClassInfoExW(hInst, kAppMenuClass, &existing)) return true;

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    wc.lpfnWndProc   = AppMenuWindow::WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kAppMenuClass;
    return RegisterClassExW(&wc) != 0;
}

LRESULT CALLBACK AppMenuWindow::WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    AppMenuWindow* pThis = nullptr;

    if (uMsg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        pThis = static_cast<AppMenuWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        pThis->hwnd_ = hwnd;
    } else {
        pThis = reinterpret_cast<AppMenuWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (pThis)
        return pThis->HandleMessage(hwnd, uMsg, wParam, lParam);
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

// ---------- geometry helpers ----------

static bool IsHorizBar(TaskbarPosition pos)
{
    return pos == TaskbarPosition::Top    ||
           pos == TaskbarPosition::Bottom ||
           pos == TaskbarPosition::Floating;
}

void AppMenuWindow::BuildEntryRects(int menuW)
{
    if (!entries_ || !settings_) return;

    entryRects_.clear();
    entryRects_.reserve(entries_->size());

    bool isList = (settings_->appMenuLayout == AppMenuLayout::List);

    if (isList) {
        int entryH = Scale(settings_->appMenuEntryHeight, dpi_);
        int y = 0;
        for (size_t i = 0; i < entries_->size(); ++i) {
            entryRects_.push_back({ 0, y, menuW, y + entryH });
            y += entryH;
        }
    } else {
        // Grid
        int cols     = std::max(1, settings_->appMenuGridCols);
        int cellW    = menuW / cols;
        int iconPx   = Scale(48, dpi_);
        int namePad  = Scale(2, dpi_);
        int nameFontH= MulDiv(settings_->appMenuGridFontSize, dpi_, 72) + Scale(4, dpi_);
        int cellH    = iconPx + namePad + nameFontH + namePad;
        int row = 0, col = 0;
        for (size_t i = 0; i < entries_->size(); ++i) {
            int x = col * cellW;
            int y = row * cellH;
            entryRects_.push_back({ x, y, x + cellW, y + cellH });
            ++col;
            if (col >= cols) { col = 0; ++row; }
        }
    }
}

void AppMenuWindow::UpdateMaxScroll(int contentH, int clientH)
{
    maxScrollOffset_ = std::max(0, contentH - clientH);
    scrollOffset_    = std::min(scrollOffset_, maxScrollOffset_);
}

// Returns the content height in pixels
static int ContentHeight(const std::vector<RECT>& rects)
{
    if (rects.empty()) return 0;
    return rects.back().bottom;
}

// ---------- painting ----------

void AppMenuWindow::Paint(HDC hdc, int w, int h)
{
    if (!entries_ || !settings_) return;

    bool isList = (settings_->appMenuLayout == AppMenuLayout::List);

    // Background
    RECT bg = { 0, 0, w, h };
    HBRUSH bgBrush = CreateSolidBrush(colors_.menuBg);
    FillRect(hdc, &bg, bgBrush);
    DeleteObject(bgBrush);

    int fontPt = isList ? settings_->appMenuListFontSize
                        : settings_->appMenuGridFontSize;
    LOGFONTW lf = {};
    lf.lfHeight  = -MulDiv(fontPt, dpi_, 72);
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    HFONT font    = CreateFontIndirectW(&lf);
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, font));
    SetBkMode(hdc, TRANSPARENT);

    int pad     = Scale(6, dpi_);
    int iconPx  = isList ? Scale(20, dpi_) : Scale(48, dpi_);

    for (int i = 0; i < static_cast<int>(entries_->size()); ++i) {
        const AppEntry& entry = (*entries_)[i];
        RECT r = entryRects_[i];

        // Apply scroll offset
        OffsetRect(&r, 0, -scrollOffset_);

        // Clip to visible area
        if (r.bottom <= 0 || r.top >= h) continue;

        // Hover highlight
        if (i == hoveredIdx_) {
            HBRUSH hb = CreateSolidBrush(colors_.menuHover);
            FillRect(hdc, &r, hb);
            DeleteObject(hb);
        }

        if (isList) {
            // Draw icon left-aligned, vertically centered
            int rowH   = r.bottom - r.top;
            int iconY  = r.top + (rowH - iconPx) / 2;
            if (entry.icon)
                DrawIconEx(hdc, r.left + pad, iconY,
                           entry.icon, iconPx, iconPx, 0, nullptr, DI_NORMAL);

            // Draw name
            RECT textR = { r.left + pad + iconPx + pad, r.top,
                           r.right - pad, r.bottom };
            SetTextColor(hdc, colors_.menuText);
            DrawTextW(hdc, entry.name.c_str(), -1, &textR,
                      DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        } else {
            // Grid: icon centered horizontally, name below
            int cellW  = r.right  - r.left;
            int iconX  = r.left + (cellW - iconPx) / 2;
            int iconY  = r.top  + Scale(4, dpi_);
            if (entry.icon)
                DrawIconEx(hdc, iconX, iconY,
                           entry.icon, iconPx, iconPx, 0, nullptr, DI_NORMAL);

            int namePad = Scale(2, dpi_);
            RECT textR = { r.left + namePad,
                           iconY + iconPx + namePad,
                           r.right - namePad,
                           r.bottom };
            SetTextColor(hdc, colors_.menuText);
            DrawTextW(hdc, entry.name.c_str(), -1, &textR,
                      DT_CENTER | DT_NOPREFIX | DT_END_ELLIPSIS | DT_WORDBREAK);
        }
    }

    // Thin scroll indicator on the right edge
    if (maxScrollOffset_ > 0) {
        double ratio = static_cast<double>(scrollOffset_) / maxScrollOffset_;
        int barH = std::max(Scale(20, dpi_), h * h / (h + maxScrollOffset_));
        int barY = static_cast<int>(ratio * (h - barH));
        RECT barR = { w - Scale(3, dpi_), barY, w, barY + barH };
        HBRUSH barBr = CreateSolidBrush(colors_.separator);
        FillRect(hdc, &barR, barBr);
        DeleteObject(barBr);
    }

    SelectObject(hdc, oldFont);
    DeleteObject(font);
}

// ---------- hit testing ----------

int AppMenuWindow::HitTestEntry(POINT ptClient) const
{
    // ptClient is in window coords; content is offset by scrollOffset_
    int contentY = ptClient.y + scrollOffset_;

    for (int i = 0; i < static_cast<int>(entryRects_.size()); ++i) {
        POINT cp = { ptClient.x, contentY };
        if (PtInRect(&entryRects_[i], cp)) return i;
    }
    return -1;
}

// ---------- launch ----------

void AppMenuWindow::LaunchEntry(int idx)
{
    if (!entries_ || idx < 0 || idx >= static_cast<int>(entries_->size())) return;
    const AppEntry& e = (*entries_)[idx];
    const std::wstring& path = e.exePath.empty() ? e.iconPath : e.exePath;
    if (!path.empty())
        ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------- scrolling ----------

void AppMenuWindow::Scroll(int pixelDelta)
{
    scrollOffset_ = std::max(0, std::min(scrollOffset_ + pixelDelta, maxScrollOffset_));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------- message handler ----------

LRESULT AppMenuWindow::HandleMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
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
        int idx = HitTestEntry(pt);
        if (idx != hoveredIdx_) {
            hoveredIdx_ = idx;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int idx = HitTestEntry(pt);
        if (idx >= 0) {
            LaunchEntry(idx);
            done_ = true;
            DestroyWindow(hwnd);
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        int step;
        if (settings_ && settings_->appMenuLayout == AppMenuLayout::Grid &&
            settings_->appMenuGridCols > 0 && !entryRects_.empty())
        {
            // Scroll by one grid row at a time
            int cols = std::max(1, settings_->appMenuGridCols);
            step = (static_cast<int>(entryRects_.size()) >= cols)
                       ? entryRects_[cols - 1].bottom
                       : entryRects_.back().bottom;
        } else {
            step = Scale(settings_ ? settings_->appMenuEntryHeight : 36, dpi_);
        }
        Scroll(delta > 0 ? -step : step);
        return 0;
    }

    case WM_KEYDOWN:
        switch (wParam) {
        case VK_ESCAPE:
            done_ = true;
            DestroyWindow(hwnd);
            break;
        case VK_UP:
            if (hoveredIdx_ > 0) {
                --hoveredIdx_;
                // Scroll to keep hovered item visible
                if (!entryRects_.empty()) {
                    int itemTop = entryRects_[hoveredIdx_].top;
                    if (itemTop < scrollOffset_)
                        scrollOffset_ = itemTop;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case VK_DOWN:
            if (entries_ && hoveredIdx_ < static_cast<int>(entries_->size()) - 1) {
                ++hoveredIdx_;
                if (!entryRects_.empty()) {
                    int itemBottom = entryRects_[hoveredIdx_].bottom;
                    if (itemBottom - scrollOffset_ > menuH_)
                        scrollOffset_ = itemBottom - menuH_;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case VK_RETURN:
            if (hoveredIdx_ >= 0) {
                LaunchEntry(hoveredIdx_);
                done_ = true;
                DestroyWindow(hwnd);
            }
            break;
        }
        return 0;

    case WM_KILLFOCUS:
        done_ = true;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        done_ = true;
        return 0;
    }

    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

// ---------- static Show ----------

void AppMenuWindow::Show(HWND hwndOwner, RECT startBtnScreenRect,
                          TaskbarPosition position,
                          std::vector<AppEntry> entries,
                          const Settings& settings,
                          const ThemeColors& colors, int dpi)
{
    if (entries.empty()) return;

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    if (!RegisterWndClass(hInst)) return;

    AppMenuWindow menu;
    // Own a snapshot: no pointers back into caller's mutable state
    menu.ownedEntries_  = std::move(entries);
    menu.ownedSettings_ = settings;
    menu.entries_       = &menu.ownedEntries_;
    menu.settings_      = &menu.ownedSettings_;
    menu.colors_        = colors;
    menu.dpi_           = dpi;

    int menuW = Scale(settings.appMenuWidth, dpi);
    menu.menuW_ = menuW;

    menu.BuildEntryRects(menuW);
    int contentH = ContentHeight(menu.entryRects_);

    // For grid mode, cap height at appMenuGridRows visible rows; otherwise use maxHeight.
    int maxH;
    if (settings.appMenuLayout == AppMenuLayout::Grid &&
        !menu.entryRects_.empty() && settings.appMenuGridCols > 0)
    {
        // Compute one row height from the first rect
        int cols = std::max(1, settings.appMenuGridCols);
        int rowH = 0;
        if (static_cast<int>(menu.entryRects_.size()) >= cols)
            rowH = menu.entryRects_[cols - 1].bottom; // first full row bottom
        else if (!menu.entryRects_.empty())
            rowH = menu.entryRects_.back().bottom;
        int maxRows = std::max(1, settings.appMenuGridRows);
        maxH = std::min(Scale(settings.appMenuMaxHeight, dpi), rowH * maxRows);
    } else {
        maxH = Scale(settings.appMenuMaxHeight, dpi);
    }

    int menuH = std::min(contentH, maxH);
    menu.menuH_ = menuH;
    menu.UpdateMaxScroll(contentH, menuH);

    // Position the menu relative to start button and taskbar edge.
    // Use the monitor that contains the start button for correct work area.
    HMONITOR hMon = MonitorFromRect(&startBtnScreenRect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfo(hMon, &mi);
    RECT workArea = mi.rcWork;

    int x, y;
    bool horizBar = IsHorizBar(position);

    if (horizBar) {
        x = startBtnScreenRect.left;
        if (position == TaskbarPosition::Top)
            y = startBtnScreenRect.bottom;
        else
            y = startBtnScreenRect.top - menuH;
    } else {
        y = startBtnScreenRect.top;
        if (position == TaskbarPosition::Left)
            x = startBtnScreenRect.right;
        else
            x = startBtnScreenRect.left - menuW;
    }

    // Clamp to work area
    if (x + menuW > workArea.right)  x = workArea.right  - menuW;
    if (y + menuH > workArea.bottom) y = workArea.bottom - menuH;
    if (x < workArea.left) x = workArea.left;
    if (y < workArea.top)  y = workArea.top;

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST,
        kAppMenuClass, nullptr,
        WS_POPUP | WS_BORDER,
        x, y, menuW, menuH,
        hwndOwner, nullptr, hInst, &menu);

    if (!hwnd) return;

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetForegroundWindow(hwnd);

    MSG msg = {};
    while (!menu.done_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // If a click arrives on any window OTHER than this popup, it means
        // the user clicked outside (including clicking the taskbar owner, which
        // does NOT trigger WM_KILLFOCUS on an owned WS_POPUP — they share an
        // activation group). Close the popup and re-dispatch the click so the
        // owner can act on it (e.g. not re-opening the menu).
        if ((msg.message == WM_LBUTTONDOWN || msg.message == WM_RBUTTONDOWN) &&
            msg.hwnd != hwnd)
        {
            menu.done_ = true;
            if (IsWindow(hwnd)) DestroyWindow(hwnd);
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    // Re-post WM_QUIT so the outer message loop can exit cleanly.
    if (msg.message == WM_QUIT)
        PostQuitMessage(static_cast<int>(msg.wParam));

    if (IsWindow(hwnd)) DestroyWindow(hwnd);
}
