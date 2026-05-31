#include "AppMenuWindow.h"
#include "LaunchHelper.h"
#include "Dpi.h"
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <powrprof.h>
#include <algorithm>

#pragma comment(lib, "PowrProf.lib")

static constexpr wchar_t kAppMenuClass[] = L"WinzooAppMenu";

// Helper to launch an app with optional same-monitor hint
static void LaunchMenuApp(HWND hwndHint, bool useHint, const wchar_t* exe, const wchar_t* args = nullptr, int nShow = SW_SHOWNORMAL)
{
    if (useHint && hwndHint) {
        HMONITOR hMon = MonitorFromWindow(hwndHint, MONITOR_DEFAULTTONEAREST);
        LaunchOnMonitor(GetModuleHandleW(nullptr), hMon, exe, args, nShow);
        return;
    }
    ShellExecuteW(nullptr, L"open", exe, args, nullptr, nShow);
}

// ---------- cached power options ----------

static std::vector<PowerOption> s_powerOptions;

void AppMenuWindow::CachePowerOptions()
{
    s_powerOptions.clear();
    s_powerOptions.push_back({ L"Lock",      PowerOption::Lock });
    s_powerOptions.push_back({ L"Sign out",  PowerOption::SignOut });

    SYSTEM_POWER_CAPABILITIES caps = {};
    if (GetPwrCapabilities(&caps)) {
        if (caps.SystemS1 || caps.SystemS2 || caps.SystemS3)
            s_powerOptions.push_back({ L"Sleep",     PowerOption::Sleep });
        if (caps.HiberFilePresent && caps.SystemS4)
            s_powerOptions.push_back({ L"Hibernate", PowerOption::Hibernate });
    }

    s_powerOptions.push_back({ L"Restart",   PowerOption::Restart });
    s_powerOptions.push_back({ L"Shut down", PowerOption::Shutdown });
}

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

static void FlattenTreeInto(const std::vector<AppTreeNode>& nodes, std::vector<AppTreeNode>& flat)
{
    for (const auto& n : nodes) {
        if (n.isFolder)
            FlattenTreeInto(n.children, flat);
        else
            flat.push_back(n);
    }
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
    wc.style         = CS_DROPSHADOW;
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

    // Scroll indicator (only in content area)
    if (maxScrollOffset_ > 0) {
        double ratio = static_cast<double>(scrollOffset_) / maxScrollOffset_;
        int barH = std::max(Scale(20, dpi_), h * h / (h + maxScrollOffset_));
        int barY = static_cast<int>(ratio * (h - barH));
        RECT barR = { menuW_ - Scale(3, dpi_), barY, menuW_, barY + barH };
        HBRUSH barBr = CreateSolidBrush(colors_.separator);
        FillRect(hdc, &barR, barBr);
        DeleteObject(barBr);
    }

    // Sidebar strip
    if (sidebarW_ > 0 && settings_) {
        // Background: slightly lighter/darker than main menu
        COLORREF sidebarBg = RGB(
            std::min(255, GetRValue(colors_.menuBg) + 12),
            std::min(255, GetGValue(colors_.menuBg) + 12),
            std::min(255, GetBValue(colors_.menuBg) + 12));
        RECT sidebarR = { menuW_, 0, w, h };
        HBRUSH sbBrush = CreateSolidBrush(sidebarBg);
        FillRect(hdc, &sidebarR, sbBrush);
        DeleteObject(sbBrush);

        // Separator line
        RECT sepR = { menuW_, 0, menuW_ + Scale(1, dpi_), h };
        HBRUSH sepBrush = CreateSolidBrush(colors_.separator);
        FillRect(hdc, &sepR, sepBrush);
        DeleteObject(sepBrush);

        // Collect visible buttons (bottom-aligned order: Explorer, Settings, Power)
        struct BtnDef { int idx; const wchar_t* glyph; };
        BtnDef btns[3];
        int btnCount = 0;
        if (settings_->appMenuSidebarShowExplorer) btns[btnCount++] = { 0, L"\uE8B7" };
        if (settings_->appMenuSidebarShowSettings) btns[btnCount++] = { 1, L"\uE713" };
        if (settings_->appMenuSidebarShowPower)    btns[btnCount++] = { 2, L"\uE7E8" };

        int btnH = Scale(settings_->appMenuEntryHeight, dpi_);
        int btnX = menuW_;
        int btnW = sidebarW_;

        // Draw buttons bottom-up
        LOGFONTW lf2 = {};
        // Size the glyph to ~2/3 of the smaller button dimension, in pixels.
        // lfHeight is negative → character height in pixels (no extra DPI scaling).
        int dim     = std::min(sidebarW_, btnH);
        lf2.lfHeight  = -std::max(10, dim * 2 / 3);
        lf2.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf2.lfFaceName, L"Segoe MDL2 Assets");
        HFONT iconFont    = CreateFontIndirectW(&lf2);
        HFONT prevFont    = static_cast<HFONT>(SelectObject(hdc, iconFont));

        for (int bi = 0; bi < btnCount; ++bi) {
            int btnY = h - (btnCount - bi) * btnH;
            if (btnY < 0) continue;  // clip if menu is shorter than button stack
            RECT btnR = { btnX, btnY, btnX + btnW, btnY + btnH };

            // Hover highlight
            if (btns[bi].idx == sidebarHoveredBtn_) {
                HBRUSH hb = CreateSolidBrush(colors_.menuHover);
                FillRect(hdc, &btnR, hb);
                DeleteObject(hb);
            }

            SetTextColor(hdc, colors_.menuText);
            DrawTextW(hdc, btns[bi].glyph, 1, &btnR,
                      DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
        }

        SelectObject(hdc, prevFont);
        DeleteObject(iconFont);
    }

    SelectObject(hdc, oldFont);
    DeleteObject(font);
}

// ---------- hit testing ----------

int AppMenuWindow::HitTestEntry(POINT ptClient) const
{
    if (ptClient.x >= menuW_) return -1;  // in sidebar
    int contentY = ptClient.y + scrollOffset_;
    for (int i = 0; i < static_cast<int>(entryRects_.size()); ++i) {
        POINT cp = { ptClient.x, contentY };
        if (PtInRect(&entryRects_[i], cp)) return i;
    }
    return -1;
}

int AppMenuWindow::HitTestSidebarBtn(POINT ptClient) const
{
    if (sidebarW_ <= 0 || !settings_) return -1;
    if (ptClient.x < menuW_) return -1;

    // Collect enabled buttons in display order (Explorer=0, Settings=1, Power=2)
    int enabled[3];
    int enabledCount = 0;
    if (settings_->appMenuSidebarShowExplorer) enabled[enabledCount++] = 0;
    if (settings_->appMenuSidebarShowSettings) enabled[enabledCount++] = 1;
    if (settings_->appMenuSidebarShowPower)    enabled[enabledCount++] = 2;

    if (enabledCount == 0) return -1;

    int btnH = Scale(settings_->appMenuEntryHeight, dpi_);
    for (int bi = 0; bi < enabledCount; ++bi) {
        int btnY = menuH_ - (enabledCount - bi) * btnH;
        if (btnY < 0) continue;  // off-screen
        if (ptClient.y >= btnY && ptClient.y < btnY + btnH)
            return enabled[bi];
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

        // When flattening submenus, show only leaf apps from the folder's tree.
        std::vector<AppTreeNode> childNodes;
        if (settings_ && settings_->appMenuFlattenMode == AppMenuFlattenMode::Submenus) {
            FlattenTreeInto(node.children, childNodes);
            std::sort(childNodes.begin(), childNodes.end(), [](const AppTreeNode& a, const AppTreeNode& b) {
                return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
            });
        } else {
            childNodes = node.children;  // copied; parent retains original
        }

        RECT nodeScreenRect = GetNodeScreenRect(idx);
        AppMenuCloseReason reason = ShowNodes(
            hwnd_, nodeScreenRect, /*isSubmenu=*/true, position_,
            std::move(childNodes),
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
            LaunchMenuApp(hwnd_, settings_->openAppsOnSameMonitor, path.c_str());
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

// ---------- sidebar button activation ----------

static void ExecutePowerAction(PowerOption::Action action)
{
    switch (action) {
    case PowerOption::Lock:
        LockWorkStation();
        break;
    case PowerOption::SignOut:
        ExitWindowsEx(EWX_LOGOFF | EWX_FORCEIFHUNG, 0);
        break;
    case PowerOption::Sleep:
        SetSuspendState(FALSE, FALSE, FALSE);
        break;
    case PowerOption::Hibernate:
        SetSuspendState(TRUE, FALSE, FALSE);
        break;
    case PowerOption::Restart:
        ShellExecuteW(nullptr, L"open", L"shutdown.exe", L"/r /t 0", nullptr, SW_HIDE);
        break;
    case PowerOption::Shutdown:
        ShellExecuteW(nullptr, L"open", L"shutdown.exe", L"/s /t 0", nullptr, SW_HIDE);
        break;
    }
}

void AppMenuWindow::ActivateSidebarBtn(int idx)
{
    if (idx == 0) {
        // Open Windows Explorer
        LaunchMenuApp(hwnd_, settings_->openAppsOnSameMonitor, L"explorer.exe");
        closeReason_ = AppMenuCloseReason::Selection;
        done_ = true;
        DestroyWindow(hwnd_);
    } else if (idx == 1) {
        // Open Windows Settings
        LaunchMenuApp(hwnd_, settings_->openAppsOnSameMonitor, L"ms-settings:");
        closeReason_ = AppMenuCloseReason::Selection;
        done_ = true;
        DestroyWindow(hwnd_);
    } else if (idx == 2) {
        // Show power menu
        if (s_powerOptions.empty()) return;

        HMENU hMenu = CreatePopupMenu();
        for (int i = 0; i < static_cast<int>(s_powerOptions.size()); ++i)
            AppendMenuW(hMenu, MF_STRING, i + 1, s_powerOptions[i].label.c_str());

        // Position popup at the top-right of the power button
        int btnH = settings_ ? Scale(settings_->appMenuEntryHeight, dpi_) : Scale(36, dpi_);
        int enabledCount = 0;
        if (settings_) {
            if (settings_->appMenuSidebarShowExplorer) ++enabledCount;
            if (settings_->appMenuSidebarShowSettings) ++enabledCount;
            if (settings_->appMenuSidebarShowPower)    ++enabledCount;
        }
        // Power button is the last button; find its top-left screen coordinate
        POINT btnPt = { menuW_, menuH_ - btnH };
        ClientToScreen(hwnd_, &btnPt);

        suppressKillFocus_ = true;
        int cmd = static_cast<int>(TrackPopupMenu(hMenu,
            TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_RIGHTALIGN | TPM_BOTTOMALIGN,
            btnPt.x, btnPt.y, 0, hwnd_, nullptr));
        suppressKillFocus_ = false;
        DestroyMenu(hMenu);

        if (cmd > 0) {
            ExecutePowerAction(s_powerOptions[cmd - 1].action);
            closeReason_ = AppMenuCloseReason::Selection;
            done_ = true;
            if (IsWindow(hwnd_)) DestroyWindow(hwnd_);
        } else {
            // User dismissed without selecting — restore focus so the menu stays active.
            if (IsWindow(hwnd_)) SetForegroundWindow(hwnd_);
        }
    }
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
        // Double-buffer: compose the full frame offscreen, then blit atomically.
        // This eliminates flicker from partial-frame draws (background → icons → text).
        HDC     memDC  = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(memDC, memBmp));
        Paint(memDC, rc.right, rc.bottom);
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int newIdx = HitTestEntry(pt);
        int newSidebarBtn = HitTestSidebarBtn(pt);
        if (newIdx != hoveredIdx_ || newSidebarBtn != sidebarHoveredBtn_) {
            hoveredIdx_      = newIdx;
            sidebarHoveredBtn_ = newSidebarBtn;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int sidebarBtn = HitTestSidebarBtn(pt);
        if (sidebarBtn >= 0) {
            ActivateSidebarBtn(sidebarBtn);
        } else {
            int idx = HitTestEntry(pt);
            if (idx >= 0)
                ActivateNode(idx);
        }
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
        // Suppress while power popup is open.
        if (suppressKillFocus_) return 0;
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

    // Sidebar: only on root menu (not submenus), and only when enabled
    int sidebarW = (!isSubmenu && settings.appMenuSidebarEnabled)
                       ? Scale(settings.appMenuSidebarWidth, dpi) : 0;
    menu.sidebarW_ = sidebarW;
    int totalW = menuW + sidebarW;

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
                    : anchorRect.left - totalW - margin;
        }
    } else {
        // Submenu: open to the right of the folder item; flip left if needed.
        x = anchorRect.right + margin;
        y = anchorRect.top;
        if (x + totalW > workArea.right)
            x = anchorRect.left - totalW - margin;
    }

    // Clamp to work area.
    if (x + totalW > workArea.right)  x = workArea.right  - totalW;
    if (y + menuH > workArea.bottom) y = workArea.bottom - menuH;
    if (x < workArea.left) x = workArea.left;
    if (y < workArea.top)  y = workArea.top;

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST,
        kAppMenuClass, nullptr,
        WS_POPUP | WS_BORDER,
        x, y, totalW, menuH,
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

    if (settings.appMenuFlattenMode == AppMenuFlattenMode::All) {
        std::vector<AppTreeNode> flat;
        FlattenTreeInto(tree, flat);
        std::sort(flat.begin(), flat.end(), [](const AppTreeNode& a, const AppTreeNode& b) {
            return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
        });
        tree = std::move(flat);
    }

    ShowNodes(hwndOwner, startBtnScreenRect, /*isSubmenu=*/false, position,
              std::move(tree), settings, colors, dpi);
}
