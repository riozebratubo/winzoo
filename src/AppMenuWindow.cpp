#include "AppMenuWindow.h"
#include "Dpi.h"
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>

static constexpr wchar_t kAppMenuClass[] = L"WinzooAppMenu";

// ---------- tree building ----------

static void SortTreeLevel(std::vector<AppTreeNode>& nodes)
{
    std::sort(nodes.begin(), nodes.end(), [](const AppTreeNode& a, const AppTreeNode& b) {
        if (a.isFolder != b.isFolder) return a.isFolder > b.isFolder; // folders first
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    for (auto& n : nodes)
        if (n.isFolder) SortTreeLevel(n.children);
}

static std::vector<AppTreeNode> BuildAppTree(const std::vector<AppEntry>& entries)
{
    std::vector<AppTreeNode> roots;

    for (const auto& entry : entries) {
        std::vector<AppTreeNode>* current = &roots;

        for (const auto& folder : entry.folderPath) {
            auto it = std::find_if(current->begin(), current->end(),
                [&folder](const AppTreeNode& n) {
                    return n.isFolder && _wcsicmp(n.name.c_str(), folder.c_str()) == 0;
                });
            if (it == current->end()) {
                AppTreeNode fn;
                fn.name     = folder;
                fn.isFolder = true;
                current->push_back(std::move(fn));
                it = current->end() - 1;
            }
            current = &it->children;
        }

        AppTreeNode leaf;
        leaf.name     = entry.name;
        leaf.isFolder = false;
        leaf.icon     = entry.icon;
        leaf.exePath  = entry.exePath;
        leaf.iconPath = entry.iconPath;
        current->push_back(std::move(leaf));
    }

    SortTreeLevel(roots);
    return roots;
}

// ---------- folder icon helper ----------

static HICON LoadFolderIcon(bool large)
{
    SHSTOCKICONINFO sii = { sizeof(sii) };
    UINT flags = SHGSI_ICON | (large ? SHGSI_LARGEICON : SHGSI_SMALLICON);
    if (SUCCEEDED(SHGetStockIconInfo(SIID_FOLDER, flags, &sii)))
        return sii.hIcon;
    return nullptr;
}

// ---------- window class ----------

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
    if (!nodes_ || !settings_) return;

    entryRects_.clear();
    entryRects_.reserve(nodes_->size());

    int padPx = Scale(settings_->appMenuPadding, dpi_);
    bool isList = (settings_->appMenuLayout == AppMenuLayout::List);

    if (isList) {
        int entryH = Scale(settings_->appMenuEntryHeight, dpi_);
        int y = padPx;
        for (size_t i = 0; i < nodes_->size(); ++i) {
            entryRects_.push_back({ padPx, y, menuW - padPx, y + entryH });
            y += entryH;
        }
    } else {
        int cols      = std::max(1, settings_->appMenuGridCols);
        int availW    = std::max(1, menuW - 2 * padPx);
        int cellW     = availW / cols;
        int iconPx    = Scale(48, dpi_);
        int namePad   = Scale(2, dpi_);
        int nameFontH = MulDiv(settings_->appMenuGridFontSize, dpi_, 72) + Scale(4, dpi_);
        int cellH     = iconPx + namePad + nameFontH + namePad;
        int row = 0, col = 0;
        for (size_t i = 0; i < nodes_->size(); ++i) {
            int x = padPx + col * cellW;
            int y = padPx + row * cellH;
            entryRects_.push_back({ x, y, x + cellW, y + cellH });
            if (++col >= cols) { col = 0; ++row; }
        }
    }
}

void AppMenuWindow::UpdateMaxScroll(int contentH, int clientH)
{
    maxScrollOffset_ = std::max(0, contentH - clientH);
    scrollOffset_    = std::min(scrollOffset_, maxScrollOffset_);
}

static int ContentHeight(const std::vector<RECT>& rects)
{
    return rects.empty() ? 0 : rects.back().bottom;
}

RECT AppMenuWindow::GetNodeScreenRect(int idx) const
{
    if (idx < 0 || idx >= static_cast<int>(entryRects_.size())) return {};
    RECT r = entryRects_[idx];
    OffsetRect(&r, 0, -scrollOffset_);
    POINT tl = { r.left, r.top };
    POINT br = { r.right, r.bottom };
    ClientToScreen(hwnd_, &tl);
    ClientToScreen(hwnd_, &br);
    return { tl.x, tl.y, br.x, br.y };
}

// ---------- painting ----------

void AppMenuWindow::Paint(HDC hdc, int w, int h)
{
    if (!nodes_ || !settings_) return;

    bool isList = (settings_->appMenuLayout == AppMenuLayout::List);

    // Load folder icons lazily (once per window instance).
    if (isList  && !folderIconList_) folderIconList_ = LoadFolderIcon(false);
    if (!isList && !folderIconGrid_) folderIconGrid_ = LoadFolderIcon(true);
    HICON folderIcon = isList ? folderIconList_ : folderIconGrid_;

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

    int pad    = Scale(6, dpi_);
    int iconPx = isList ? Scale(20, dpi_) : Scale(48, dpi_);

    for (int i = 0; i < static_cast<int>(nodes_->size()); ++i) {
        const AppTreeNode& node = (*nodes_)[i];
        RECT r = entryRects_[i];
        OffsetRect(&r, 0, -scrollOffset_);

        if (r.bottom <= 0 || r.top >= h) continue;

        // Hover highlight
        if (i == hoveredIdx_) {
            HBRUSH hb = CreateSolidBrush(colors_.menuHover);
            FillRect(hdc, &r, hb);
            DeleteObject(hb);
        }

        HICON drawIcon = node.isFolder ? folderIcon : node.icon;

        if (isList) {
            int rowH  = r.bottom - r.top;
            int iconY = r.top + (rowH - iconPx) / 2;
            if (drawIcon)
                DrawIconEx(hdc, r.left + pad, iconY,
                           drawIcon, iconPx, iconPx, 0, nullptr, DI_NORMAL);

            // Reserve space for the folder chevron on the right.
            int chevW  = node.isFolder ? Scale(16, dpi_) : 0;
            RECT textR = { r.left + pad + iconPx + pad, r.top,
                           r.right - pad - chevW,        r.bottom };
            SetTextColor(hdc, colors_.menuText);
            DrawTextW(hdc, node.name.c_str(), -1, &textR,
                      DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

            if (node.isFolder) {
                RECT chevR = { r.right - pad - chevW, r.top, r.right - pad, r.bottom };
                SetTextColor(hdc, colors_.textDimmed);
                DrawTextW(hdc, L"\u25B6", 1, &chevR,
                          DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
            }
        } else {
            // Grid: icon centred, name below.
            int cellW = r.right  - r.left;
            int iconX = r.left + (cellW - iconPx) / 2;
            int iconY = r.top  + Scale(4, dpi_);
            if (drawIcon)
                DrawIconEx(hdc, iconX, iconY,
                           drawIcon, iconPx, iconPx, 0, nullptr, DI_NORMAL);

            int namePad = Scale(2, dpi_);
            RECT textR  = { r.left + namePad,
                             iconY + iconPx + namePad,
                             r.right - namePad,
                             r.bottom };
            SetTextColor(hdc, colors_.menuText);
            DrawTextW(hdc, node.name.c_str(), -1, &textR,
                      DT_CENTER | DT_NOPREFIX | DT_END_ELLIPSIS | DT_WORDBREAK);
        }
    }

    // Scroll indicator
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
    int contentY = ptClient.y + scrollOffset_;
    for (int i = 0; i < static_cast<int>(entryRects_.size()); ++i) {
        POINT cp = { ptClient.x, contentY };
        if (PtInRect(&entryRects_[i], cp)) return i;
    }
    return -1;
}

// ---------- activate (launch app or open folder submenu) ----------

void AppMenuWindow::ActivateNode(int idx)
{
    if (!nodes_ || idx < 0 || idx >= static_cast<int>(nodes_->size())) return;
    const AppTreeNode& node = (*nodes_)[idx];

    if (node.isFolder) {
        if (node.children.empty()) return;

        RECT nodeScreenRect = GetNodeScreenRect(idx);
        AppMenuCloseReason reason = ShowNodes(
            hwnd_, nodeScreenRect, /*isSubmenu=*/true, position_,
            node.children,   // copied; parent retains original
            *settings_, colors_, dpi_,
            &subMenuHwnd_);  // parent stores child HWND for WM_KILLFOCUS guard
        subMenuHwnd_ = nullptr;

        if (reason == AppMenuCloseReason::Escape) {
            // Submenu was dismissed without a selection (Escape key, or
            // re-dispatched click on this window). Restore focus here if
            // the outer window wasn't destroyed by the re-dispatch.
            if (!done_ && IsWindow(hwnd_))
                SetForegroundWindow(hwnd_);
        } else {
            // Selection or ClickedOutside: close this level too.
            done_ = true;
            if (IsWindow(hwnd_))
                DestroyWindow(hwnd_);
        }
    } else {
        const std::wstring& path = node.exePath.empty() ? node.iconPath : node.exePath;
        if (!path.empty())
            ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        closeReason_ = AppMenuCloseReason::Selection;
        done_ = true;
        DestroyWindow(hwnd_);
    }
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
        if (idx >= 0)
            ActivateNode(idx);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        int step;
        if (settings_ && settings_->appMenuLayout == AppMenuLayout::Grid &&
            settings_->appMenuGridCols > 0 && !entryRects_.empty())
        {
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
            closeReason_ = AppMenuCloseReason::Escape;
            done_ = true;
            DestroyWindow(hwnd);
            break;
        case VK_LEFT:
            // Navigate back to parent when inside a submenu.
            if (isSubmenu_) {
                closeReason_ = AppMenuCloseReason::Escape;
                done_ = true;
                DestroyWindow(hwnd);
            }
            break;
        case VK_RIGHT:
            // Open submenu if the hovered item is a folder.
            if (hoveredIdx_ >= 0 && nodes_ &&
                hoveredIdx_ < static_cast<int>(nodes_->size()) &&
                (*nodes_)[hoveredIdx_].isFolder)
            {
                ActivateNode(hoveredIdx_);
            }
            break;
        case VK_UP:
            if (hoveredIdx_ > 0) {
                --hoveredIdx_;
                if (!entryRects_.empty()) {
                    int top = entryRects_[hoveredIdx_].top;
                    if (top < scrollOffset_) scrollOffset_ = top;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case VK_DOWN:
            if (nodes_ && hoveredIdx_ < static_cast<int>(nodes_->size()) - 1) {
                ++hoveredIdx_;
                if (!entryRects_.empty()) {
                    int bot = entryRects_[hoveredIdx_].bottom;
                    if (bot - scrollOffset_ > menuH_) scrollOffset_ = bot - menuH_;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case VK_RETURN:
            if (hoveredIdx_ >= 0)
                ActivateNode(hoveredIdx_);
            break;
        }
        return 0;

    case WM_KILLFOCUS:
        // Suppress if focus moved to our active child submenu.
        if (subMenuHwnd_ && (HWND)wParam == subMenuHwnd_) return 0;
        done_ = true;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        done_ = true;
        if (folderIconList_) { DestroyIcon(folderIconList_); folderIconList_ = nullptr; }
        if (folderIconGrid_) { DestroyIcon(folderIconGrid_); folderIconGrid_ = nullptr; }
        return 0;
    }

    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

// ---------- ShowNodes (internal, used for root and submenus) ----------

AppMenuCloseReason AppMenuWindow::ShowNodes(
    HWND hwndOwner, RECT anchorRect, bool isSubmenu,
    TaskbarPosition position,
    std::vector<AppTreeNode> nodes,
    const Settings& settings,
    const ThemeColors& colors, int dpi,
    HWND* pChildHwnd)
{
    if (nodes.empty()) return AppMenuCloseReason::ClickedOutside;

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    if (!RegisterWndClass(hInst)) return AppMenuCloseReason::ClickedOutside;

    AppMenuWindow menu;
    menu.ownedNodes_    = std::move(nodes);
    menu.ownedSettings_ = settings;
    menu.nodes_         = &menu.ownedNodes_;
    menu.settings_      = &menu.ownedSettings_;
    menu.colors_        = colors;
    menu.dpi_           = dpi;
    menu.position_      = position;
    menu.isSubmenu_     = isSubmenu;

    int menuW = Scale(settings.appMenuWidth, dpi);
    menu.menuW_ = menuW;

    menu.BuildEntryRects(menuW);
    int padPx    = Scale(settings.appMenuPadding, dpi);
    int contentH = ContentHeight(menu.entryRects_) + padPx;

    int maxH;
    if (settings.appMenuLayout == AppMenuLayout::Grid &&
        !menu.entryRects_.empty() && settings.appMenuGridCols > 0)
    {
        int cols  = std::max(1, settings.appMenuGridCols);
        // Row height = distance between tops of consecutive rows (exclude top padding).
        int cellH = (static_cast<int>(menu.entryRects_.size()) >= cols)
                        ? menu.entryRects_[cols - 1].bottom - padPx
                        : menu.entryRects_.back().bottom    - padPx;
        int maxRows = std::max(1, settings.appMenuGridRows);
        maxH = std::min(Scale(settings.appMenuMaxHeight, dpi), 2 * padPx + cellH * maxRows);
    } else {
        maxH = Scale(settings.appMenuMaxHeight, dpi);
    }

    int menuH = std::min(contentH, maxH);
    menu.menuH_ = menuH;
    menu.UpdateMaxScroll(contentH, menuH);

    HMONITOR hMon = MonitorFromRect(&anchorRect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfo(hMon, &mi);
    RECT workArea = mi.rcWork;

    int x, y;
    int margin = Scale(settings.appMenuMargin, dpi);
    if (!isSubmenu) {
        // Root menu: anchor to start button / taskbar edge.
        bool horizBar = IsHorizBar(position);
        if (horizBar) {
            x = anchorRect.left;
            y = (position == TaskbarPosition::Top)
                    ? anchorRect.bottom + margin
                    : anchorRect.top - menuH - margin;
        } else {
            y = anchorRect.top;
            x = (position == TaskbarPosition::Left)
                    ? anchorRect.right + margin
                    : anchorRect.left - menuW - margin;
        }
    } else {
        // Submenu: open to the right of the folder item; flip left if needed.
        x = anchorRect.right + margin;
        y = anchorRect.top;
        if (x + menuW > workArea.right)
            x = anchorRect.left - menuW - margin;
    }

    // Clamp to work area.
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

    if (!hwnd) return AppMenuCloseReason::ClickedOutside;

    // Expose our HWND to the parent *before* SetForegroundWindow so the parent
    // can suppress WM_KILLFOCUS caused by the focus transfer.
    if (pChildHwnd) *pChildHwnd = hwnd;

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetForegroundWindow(hwnd);

    MSG msg = {};
    while (!menu.done_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if ((msg.message == WM_LBUTTONDOWN || msg.message == WM_RBUTTONDOWN) &&
            msg.hwnd != hwnd)
        {
            // Click outside the current menu.
            bool clickedOnParent = isSubmenu && (msg.hwnd == hwndOwner);
            menu.closeReason_ = clickedOnParent
                ? AppMenuCloseReason::Escape       // let parent handle this click
                : AppMenuCloseReason::ClickedOutside;
            menu.done_ = true;
            if (IsWindow(hwnd)) DestroyWindow(hwnd);

            // Re-dispatch so:
            //  - Root menu: taskbar receives the click for toggle-guard logic.
            //  - Submenu with click on parent: parent processes the intended click.
            if (!isSubmenu || clickedOnParent) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (msg.message == WM_QUIT)
        PostQuitMessage(static_cast<int>(msg.wParam));

    if (IsWindow(hwnd)) DestroyWindow(hwnd);

    return menu.closeReason_;
}

// ---------- public Show ----------

void AppMenuWindow::Show(HWND hwndOwner, RECT startBtnScreenRect,
                          TaskbarPosition position,
                          std::vector<AppEntry> entries,
                          const Settings& settings,
                          const ThemeColors& colors, int dpi)
{
    if (entries.empty()) return;
    std::vector<AppTreeNode> tree = BuildAppTree(entries);
    if (tree.empty()) return;
    ShowNodes(hwndOwner, startBtnScreenRect, /*isSubmenu=*/false, position,
              std::move(tree), settings, colors, dpi);
}
