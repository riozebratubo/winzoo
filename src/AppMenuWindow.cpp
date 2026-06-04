#include "AppMenuWindow.h"
#include "LaunchHelper.h"
#include "SettingsSearch.h"
#include "Dpi.h"
#include "resource.h"
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <powrprof.h>
#include <algorithm>
#include <utility>

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
    SHSTOCKICONINFO sii = {};
    sii.cbSize = sizeof(sii);
    UINT flags = SHGSI_ICON | (large ? SHGSI_LARGEICON : SHGSI_SMALLICON);
    if (SUCCEEDED(SHGetStockIconInfo(SIID_FOLDER, flags, &sii)))
        return sii.hIcon;
    return nullptr;
}

// ---------- window class ----------

bool AppMenuWindow::RegisterWndClass(HINSTANCE hInst)
{
    WNDCLASSEXW existing = {};
    existing.cbSize = sizeof(existing);
    if (GetClassInfoExW(hInst, kAppMenuClass, &existing)) return true;

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
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
        int entryH    = Scale(settings_->appMenuEntryHeight, dpi_);
        int subtitleH = entryH + Scale(14, dpi_);
        int y = padPx;
        for (size_t i = 0; i < nodes_->size(); ++i) {
            int h = (*nodes_)[i].subtitle.empty() ? entryH : subtitleH;
            entryRects_.push_back({ padPx, y, menuW - padPx, y + h });
            y += h;
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
    if (idx < 0 || std::cmp_greater_equal(idx, entryRects_.size())) return {};
    RECT r = entryRects_[idx];
    OffsetRect(&r, 0, -scrollOffset_);
    POINT tl = { r.left, r.top };
    POINT br = { r.right, r.bottom };
    ClientToScreen(hwnd_, &tl);
    ClientToScreen(hwnd_, &br);
    return { tl.x, tl.y, br.x, br.y };
}

// ---------- search ----------

int AppMenuWindow::SearchBoxHeight() const
{
    if (!searchEnabled_) return 0;
    return Scale(28, dpi_);
}

// Fuzzy match: checks if all characters of the query appear in order in the name.
// Returns true if the query chars are a subsequence of name (case-insensitive).
static bool FuzzyMatch(const std::wstring& lowerName, const std::wstring& lowerQuery)
{
    size_t qi = 0;
    for (size_t ni = 0; ni < lowerName.size() && qi < lowerQuery.size(); ++ni) {
        if (lowerName[ni] == lowerQuery[qi])
            ++qi;
    }
    return qi == lowerQuery.size();
}

static void CollectLeaves(const std::vector<AppTreeNode>& nodes,
                           const std::wstring& lowerQuery,
                           bool fuzzy,
                           std::vector<AppTreeNode>& out)
{
    for (const auto& n : nodes) {
        if (n.isFolder) {
            CollectLeaves(n.children, lowerQuery, fuzzy, out);
        } else {
            std::wstring lowerName = n.name;
            for (auto& ch : lowerName) ch = towlower(ch);
            bool match = fuzzy ? FuzzyMatch(lowerName, lowerQuery)
                               : lowerName.find(lowerQuery) != std::wstring::npos;
            if (match)
                out.push_back(n);
        }
    }
}

void AppMenuWindow::ApplyFilter()
{
    if (searchText_.empty()) {
        // Restore original node list
        nodes_ = &ownedNodes_;
    } else {
        std::wstring lowerQuery = searchText_;
        for (auto& ch : lowerQuery) ch = towlower(ch);
        filteredNodes_.clear();
        bool fuzzy = settings_ && settings_->appMenuSearchFuzzy;
        CollectLeaves(ownedNodes_, lowerQuery, fuzzy, filteredNodes_);
        std::sort(filteredNodes_.begin(), filteredNodes_.end(),
                  [](const AppTreeNode& a, const AppTreeNode& b) {
                      return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
                  });

        if (lowerQuery.size() >= 2 && settings_ && settings_->appMenuSearchSystem) {
            // Settings pages — prepend so they appear above shortcut matches.
            if (!settingsIcon_) {
                wchar_t settingsExe[MAX_PATH];
                ExpandEnvironmentStringsW(
                    L"%windir%\\ImmersiveControlPanel\\SystemSettings.exe",
                    settingsExe, MAX_PATH);
                ExtractIconExW(settingsExe, 0, nullptr, &settingsIcon_, 1);
                if (!settingsIcon_)
                    settingsIcon_ = LoadIcon(nullptr, IDI_APPLICATION);
            }
            auto pages = SearchSettingsPages(lowerQuery);
            for (int pi = static_cast<int>(pages.size()) - 1; pi >= 0; --pi) {
                const SettingsPageDef* p = pages[pi];
                AppTreeNode n;
                n.name     = p->displayName;
                n.type     = AppNodeType::SettingsPage;
                n.subtitle = L"Windows Settings";
                n.exePath  = p->uri;
                n.icon     = settingsIcon_;
                filteredNodes_.insert(filteredNodes_.begin(), std::move(n));
            }

            // Executable from PATH — append below shortcut matches.
            wchar_t exeBuf[MAX_PATH];
            wcscpy_s(exeBuf, searchText_.c_str());
            bool hasExt = lowerQuery.ends_with(L".exe") ||
                          lowerQuery.ends_with(L".com") ||
                          lowerQuery.ends_with(L".bat");
            if (!hasExt) wcscat_s(exeBuf, L".exe");
            if (PathFindOnPathW(exeBuf, nullptr)) {
                std::wstring exePath(exeBuf);
                AppTreeNode n;
                // Use the filename portion as display name (gets correct casing).
                auto slash = exePath.rfind(L'\\');
                n.name     = (slash != std::wstring::npos) ? exePath.substr(slash + 1) : exePath;
                n.type     = AppNodeType::Executable;
                n.subtitle = L"Run command";
                n.exePath  = exePath;
                auto it = exeIconCache_.find(exePath);
                if (it == exeIconCache_.end()) {
                    HICON hi = nullptr;
                    ExtractIconExW(exePath.c_str(), 0, nullptr, &hi, 1);
                    exeIconCache_[exePath] = hi;
                    n.icon = hi;
                } else {
                    n.icon = it->second;
                }
                filteredNodes_.push_back(std::move(n));
            }
        }

        nodes_ = &filteredNodes_;
    }
    BuildEntryRects(menuW_);
    int padPx    = Scale(settings_ ? settings_->appMenuPadding : 6, dpi_);
    int contentH = ContentHeight(entryRects_) + padPx;
    int clientH  = menuH_ - searchBoxH_;
    UpdateMaxScroll(contentH, clientH);
    // Auto-select first result when searching so Enter opens it directly
    hoveredIdx_ = (!searchText_.empty() && nodes_ && !nodes_->empty()) ? 0 : -1;
    scrollOffset_ = 0;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// Subclass proc for the search EDIT control — forwards navigation keys to the menu.
// Uses window properties to avoid private-member access from a static function:
//   GWLP_USERDATA  — original WNDPROC
//   "WinzooMenu"   — AppMenuWindow* (set once at creation, never changes)
//   "WinzooMEF"    — non-null when user explicitly clicked the box (MEF = menu explicit focus)
static LRESULT CALLBACK SearchEditSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    WNDPROC origProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!origProc) return DefWindowProcW(hwnd, uMsg, wParam, lParam);

    HWND hParent = GetParent(hwnd);

    if (uMsg == WM_LBUTTONDOWN) {
        // User explicitly clicked the search box — record that it owns input focus.
        SetPropW(hwnd, L"WinzooMEF", reinterpret_cast<HANDLE>(1ULL));
        // Fall through to default handling.
    }

    if (uMsg == WM_KILLFOCUS) {
        RemovePropW(hwnd, L"WinzooMEF");
        HWND hNewFocus = reinterpret_cast<HWND>(wParam);
        // If focus is leaving to something other than our parent menu, close.
        if (hNewFocus != hParent)
            SendMessageW(hParent, WM_KILLFOCUS, wParam, lParam);
    }

    if (uMsg == WM_KEYDOWN) {
        bool explicitlyFocused = GetPropW(hwnd, L"WinzooMEF") != nullptr;
        switch (wParam) {
        case VK_ESCAPE:
        case VK_UP:
        case VK_DOWN:
        case VK_RETURN:
            // Forward to the parent menu for item navigation.
            // Keep OS focus on the edit so backspace/typing still work.
            return SendMessageW(hParent, uMsg, wParam, lParam);
        case VK_LEFT:
        case VK_RIGHT:
            if (!explicitlyFocused) {
                // Implicit focus — forward for grid/list navigation.
                return SendMessageW(hParent, uMsg, wParam, lParam);
            }
            // User clicked the box deliberately — let the edit move its cursor.
            break;
        default: break;
        }
    }

    return CallWindowProcW(origProc, hwnd, uMsg, wParam, lParam);
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
    int contentAreaH = h - searchBoxH_;  // area available for entries (above search box)

    // Clip drawing to the content area (above the search box)
    HRGN clipRgn = nullptr;
    if (searchBoxH_ > 0) {
        clipRgn = CreateRectRgn(0, 0, menuW_, contentAreaH);
        SelectClipRgn(hdc, clipRgn);
    }

    for (int i = 0; std::cmp_less(i, nodes_->size()); ++i) {
        const AppTreeNode& node = (*nodes_)[i];
        RECT r = entryRects_[i];
        OffsetRect(&r, 0, -scrollOffset_);

        if (r.bottom <= 0 || r.top >= contentAreaH) continue;

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

            int chevW    = node.isFolder ? Scale(16, dpi_) : 0;
            int textLeft = r.left + pad + iconPx + pad;
            int textRight = r.right - pad - chevW;

            if (!node.subtitle.empty()) {
                int split = r.top + rowH * 60 / 100;
                RECT nameR = { textLeft, r.top + Scale(2, dpi_), textRight, split };
                SetTextColor(hdc, colors_.menuText);
                DrawTextW(hdc, node.name.c_str(), -1, &nameR,
                          DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

                LOGFONTW lfSub = lf;
                lfSub.lfHeight = lfSub.lfHeight * 8 / 10;
                HFONT subFont  = CreateFontIndirectW(&lfSub);
                HFONT prevFont = static_cast<HFONT>(SelectObject(hdc, subFont));
                RECT subR = { textLeft, split, textRight, r.bottom - Scale(2, dpi_) };
                SetTextColor(hdc, colors_.textDimmed);
                DrawTextW(hdc, node.subtitle.c_str(), -1, &subR,
                          DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
                SelectObject(hdc, prevFont);
                DeleteObject(subFont);
            } else {
                RECT textR = { textLeft, r.top, textRight, r.bottom };
                SetTextColor(hdc, colors_.menuText);
                DrawTextW(hdc, node.name.c_str(), -1, &textR,
                          DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

                if (node.isFolder) {
                    RECT chevR = { r.right - pad - chevW, r.top, r.right - pad, r.bottom };
                    SetTextColor(hdc, colors_.textDimmed);
                    DrawTextW(hdc, L"\u25B6", 1, &chevR,
                              DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
                }
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
        int barH = std::max(Scale(20, dpi_), contentAreaH * contentAreaH / (contentAreaH + maxScrollOffset_));
        int barY = static_cast<int>(ratio * (contentAreaH - barH));
        RECT barR = { menuW_ - Scale(3, dpi_), barY, menuW_, barY + barH };
        HBRUSH barBr = CreateSolidBrush(colors_.separator);
        FillRect(hdc, &barR, barBr);
        DeleteObject(barBr);
    }

    // Remove clip region before drawing search box and sidebar
    if (clipRgn) {
        SelectClipRgn(hdc, nullptr);
        DeleteObject(clipRgn);
    }

    // Search box at the bottom of the content area
    if (searchBoxH_ > 0) {
        int sbY = h - searchBoxH_;
        int sbPad = Scale(4, dpi_);

        // Separator line above search box
        RECT sepLine = { 0, sbY, menuW_, sbY + 1 };
        HBRUSH sepBr = CreateSolidBrush(colors_.separator);
        FillRect(hdc, &sepLine, sepBr);
        DeleteObject(sepBr);

        // Magnifying glass icon (left of the EDIT control)
        int iconAreaW = Scale(20, dpi_);
        RECT iconBg = { 0, sbY + 1, sbPad + iconAreaW, h };
        HBRUSH iconBgBr = CreateSolidBrush(colors_.menuBg);
        FillRect(hdc, &iconBg, iconBgBr);
        DeleteObject(iconBgBr);

        LOGFONTW lfIcon = {};
        lfIcon.lfHeight  = -Scale(12, dpi_);
        lfIcon.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lfIcon.lfFaceName, L"Segoe MDL2 Assets");
        HFONT iconFont  = CreateFontIndirectW(&lfIcon);
        HFONT prevFont2 = static_cast<HFONT>(SelectObject(hdc, iconFont));
        SetTextColor(hdc, colors_.textDimmed);
        RECT iconR = { sbPad, sbY + 1, sbPad + iconAreaW, h };
        DrawTextW(hdc, L"\uE721", 1, &iconR, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        SelectObject(hdc, prevFont2);
        DeleteObject(iconFont);
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

        // Collect visible buttons (bottom-aligned order: WinzooSettings, AllApps, Explorer, Settings, Power)
        struct BtnDef { int idx; const wchar_t* glyph; };
        BtnDef btns[5];
        int btnCount = 0;
        btns[btnCount++] = { 3, L"\uE115" };  // Winzoo Settings
        btns[btnCount++] = { 4, L"\uE8F1" };  // All Apps
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
    if (searchBoxH_ > 0 && ptClient.y >= menuH_ - searchBoxH_) return -1;  // in search box
    int contentY = ptClient.y + scrollOffset_;
    for (int i = 0; std::cmp_less(i, entryRects_.size()); ++i) {
        POINT cp = { ptClient.x, contentY };
        if (PtInRect(&entryRects_[i], cp)) return i;
    }
    return -1;
}

int AppMenuWindow::HitTestSidebarBtn(POINT ptClient) const
{
    if (sidebarW_ <= 0 || !settings_) return -1;
    if (ptClient.x < menuW_) return -1;

    // Collect enabled buttons in display order
    int enabled[5];
    int enabledCount = 0;
    enabled[enabledCount++] = 3;  // Winzoo Settings (always visible)
    enabled[enabledCount++] = 4;  // All Apps (always visible)
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
    if (!nodes_ || idx < 0 || std::cmp_greater_equal(idx, nodes_->size())) return;
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
        if (!settings_) return;
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

void AppMenuWindow::EnsureVisible(int idx)
{
    if (idx < 0 || std::cmp_greater_equal(idx, entryRects_.size())) return;
    int clientH = menuH_ - searchBoxH_;
    int top = entryRects_[idx].top;
    int bot = entryRects_[idx].bottom;
    if (top < scrollOffset_)
        scrollOffset_ = top;
    else if (bot - scrollOffset_ > clientH)
        scrollOffset_ = bot - clientH;
    scrollOffset_ = std::max(0, std::min(scrollOffset_, maxScrollOffset_));
}

void AppMenuWindow::SetHoveredIdx(int idx)
{
    if (!nodes_ || idx < 0 || std::cmp_greater_equal(idx, nodes_->size())) return;
    hoveredIdx_ = idx;
    EnsureVisible(idx);
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
        for (int i = 0; std::cmp_less(i, s_powerOptions.size()); ++i)
            AppendMenuW(hMenu, MF_STRING, i + 1, s_powerOptions[i].label.c_str());

        // Position popup at the top-right of the power button
        int btnH = settings_ ? Scale(settings_->appMenuEntryHeight, dpi_) : Scale(36, dpi_);
        int enabledCount = 2;  // Winzoo Settings + All Apps always present
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
    } else if (idx == 3) {
        // Open Winzoo Settings dialog (post to owner taskbar window)
        HWND owner = GetWindow(hwnd_, GW_OWNER);
        closeReason_ = AppMenuCloseReason::Selection;
        done_ = true;
        DestroyWindow(hwnd_);
        if (owner) PostMessageW(owner, WM_COMMAND, IDM_SETTINGS, 0);
    } else if (idx == 4) {
        // Show the app background menu (Settings, Export, etc.) without closing start menu
        HMENU hMenu = CreatePopupMenu();
        AppendMenuW(hMenu, MF_STRING, IDM_SETTINGS,           L"Settings...");
        AppendMenuW(hMenu, MF_STRING, IDM_EXPORT_SETTINGS,    L"Export settings...");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, IDM_REBUILD_ICON_CACHE, L"Rebuild icon cache");
        AppendMenuW(hMenu, MF_STRING, IDM_ABOUT,              L"About Winzoo...");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, IDM_RESTART,            L"Restart");
        AppendMenuW(hMenu, MF_STRING, IDM_CLOSE_TASKBAR,      L"Close");

        // Position at the button
        int btnH = settings_ ? Scale(settings_->appMenuEntryHeight, dpi_) : Scale(36, dpi_);
        POINT btnPt = { menuW_ + sidebarW_, 0 };
        // Find the button's vertical center (second button from top)
        btnPt.y = menuH_ - (3 + (settings_->appMenuSidebarShowExplorer ? 1 : 0)
                              + (settings_->appMenuSidebarShowSettings ? 1 : 0)
                              + (settings_->appMenuSidebarShowPower ? 1 : 0)) * btnH + btnH;
        ClientToScreen(hwnd_, &btnPt);

        suppressKillFocus_ = true;
        int cmd = static_cast<int>(TrackPopupMenu(hMenu,
            TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_RIGHTALIGN | TPM_TOPALIGN,
            btnPt.x, btnPt.y, 0, hwnd_, nullptr));
        suppressKillFocus_ = false;
        DestroyMenu(hMenu);

        if (cmd > 0) {
            HWND owner = GetWindow(hwnd_, GW_OWNER);
            closeReason_ = AppMenuCloseReason::Selection;
            done_ = true;
            DestroyWindow(hwnd_);
            if (owner) PostMessageW(owner, WM_COMMAND, static_cast<WPARAM>(cmd), 0);
        } else {
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

    case WM_CREATE:
        if (searchEnabled_) {
            int sbPad    = Scale(4, dpi_);
            int iconAreaW = Scale(20, dpi_);
            int editX = sbPad + iconAreaW;
            int editY = menuH_ - searchBoxH_ + Scale(3, dpi_);
            int editW = menuW_ - editX - sbPad;
            int editH = searchBoxH_ - Scale(6, dpi_);

            searchEdit_ = CreateWindowExW(
                0, L"EDIT", nullptr,
                WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL,
                editX, editY, editW, editH,
                hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);

            if (searchEdit_) {
                // Set font
                LOGFONTW lfEdit = {};
                lfEdit.lfHeight  = -MulDiv(9, dpi_, 72);
                lfEdit.lfQuality = CLEARTYPE_QUALITY;
                wcscpy_s(lfEdit.lfFaceName, L"Segoe UI");
                HFONT editFont = CreateFontIndirectW(&lfEdit);
                SendMessageW(searchEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(editFont), TRUE);

                // Set placeholder text (cue banner)
                SendMessageW(searchEdit_, EM_SETCUEBANNER, TRUE,
                             reinterpret_cast<LPARAM>(L"Search..."));

                // Subclass to forward navigation keys to the menu
                WNDPROC origProc = reinterpret_cast<WNDPROC>(
                    SetWindowLongPtrW(searchEdit_, GWLP_WNDPROC,
                                     reinterpret_cast<LONG_PTR>(SearchEditSubclassProc)));
                SetWindowLongPtrW(searchEdit_, GWLP_USERDATA,
                                 reinterpret_cast<LONG_PTR>(origProc));
                // Store this pointer so the static subclass proc can reach us.
                SetPropW(searchEdit_, L"WinzooMenu",
                         static_cast<HANDLE>(static_cast<void*>(this)));
            }
        }
        return 0;

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
        int step = 0;
        if (settings_ && settings_->appMenuLayout == AppMenuLayout::Grid &&
            settings_->appMenuGridCols > 0 && !entryRects_.empty())
        {
            int cols = std::max(1, settings_->appMenuGridCols);
            step = std::cmp_greater_equal(entryRects_.size(), cols)
                       ? entryRects_[cols - 1].bottom
                       : entryRects_.back().bottom;
        } else {
            step = Scale(settings_ ? settings_->appMenuEntryHeight : 36, dpi_);
        }
        Scroll(delta > 0 ? -step : step);
        return 0;
    }

    case WM_KEYDOWN: {
        bool isList = !settings_ || settings_->appMenuLayout == AppMenuLayout::List;
        int cols  = (isList || !settings_) ? 1 : std::max(1, settings_->appMenuGridCols);
        int count = nodes_ ? static_cast<int>(nodes_->size()) : 0;

        switch (wParam) {
        case VK_ESCAPE:
            if (searchEnabled_ && !searchText_.empty()) {
                // First Escape clears search; second closes menu.
                searchText_.clear();
                if (searchEdit_) SetWindowTextW(searchEdit_, L"");
                ApplyFilter();
            } else {
                closeReason_ = AppMenuCloseReason::Escape;
                done_ = true;
                DestroyWindow(hwnd);
            }
            break;

        case VK_LEFT:
            if (isList) {
                // List: navigate back to parent when inside a submenu.
                if (isSubmenu_) {
                    closeReason_ = AppMenuCloseReason::Escape;
                    done_ = true;
                    DestroyWindow(hwnd);
                }
            } else {
                // Grid: move one item to the left.
                if (count == 0) break;
                if (hoveredIdx_ == -1) {
                    SetHoveredIdx(0);
                } else if (hoveredIdx_ > 0) {
                    SetHoveredIdx(hoveredIdx_ - 1);
                } else if (isSubmenu_) {
                    // At first item in a grid submenu — close it.
                    closeReason_ = AppMenuCloseReason::Escape;
                    done_ = true;
                    DestroyWindow(hwnd);
                }
            }
            break;

        case VK_RIGHT:
            if (isList) {
                // List: open submenu if the hovered item is a folder.
                if (hoveredIdx_ >= 0 && nodes_ &&
                    std::cmp_less(hoveredIdx_, nodes_->size()) &&
                    (*nodes_)[hoveredIdx_].isFolder)
                {
                    EnsureVisible(hoveredIdx_);
                    ActivateNode(hoveredIdx_);
                }
            } else {
                // Grid: move one item to the right.
                if (count == 0) break;
                if (hoveredIdx_ == -1) {
                    SetHoveredIdx(0);
                } else if (hoveredIdx_ + 1 < count) {
                    SetHoveredIdx(hoveredIdx_ + 1);
                }
            }
            break;

        case VK_UP:
            if (count == 0) break;
            if (hoveredIdx_ == -1) {
                SetHoveredIdx(count - 1);
            } else if (hoveredIdx_ - cols >= 0) {
                SetHoveredIdx(hoveredIdx_ - cols);
            }
            break;

        case VK_DOWN:
            if (count == 0) break;
            if (hoveredIdx_ == -1) {
                SetHoveredIdx(0);
            } else {
                int newIdx = hoveredIdx_ + cols;
                if (newIdx < count) {
                    SetHoveredIdx(newIdx);
                } else {
                    // In grid: if there's a row below the current one, clamp
                    // to the last item (partial last row).
                    int curRow  = hoveredIdx_ / cols;
                    int lastRow = (count - 1) / cols;
                    if (curRow < lastRow)
                        SetHoveredIdx(count - 1);
                }
            }
            break;

        case VK_RETURN:
            if (hoveredIdx_ >= 0) {
                EnsureVisible(hoveredIdx_);
                ActivateNode(hoveredIdx_);
            }
            break;
        default: break;
        }
        return 0;
    }

    case WM_CHAR:
        // When the menu itself has focus and the user types a printable char,
        // redirect focus and the keystroke into the search EDIT control.
        if (searchEnabled_ && searchEdit_ && wParam >= 0x20) {
            SetFocus(searchEdit_);
            SendMessageW(searchEdit_, uMsg, wParam, lParam);
        }
        return 0;

    case WM_COMMAND:
        // EN_CHANGE from the search EDIT control
        if (searchEdit_ && HIWORD(wParam) == EN_CHANGE &&
            reinterpret_cast<HWND>(lParam) == searchEdit_)
        {
            int len = GetWindowTextLengthW(searchEdit_);
            if (len > 0) {
                searchText_.resize(static_cast<size_t>(len));
                GetWindowTextW(searchEdit_, searchText_.data(), len + 1);
            } else {
                searchText_.clear();
            }
            ApplyFilter();
        }
        return 0;

    case WM_CTLCOLOREDIT:
        // Theme the EDIT control to match the menu colors
        if (searchEdit_ && reinterpret_cast<HWND>(lParam) == searchEdit_) {
            HDC hdcEdit = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdcEdit, colors_.menuText);
            COLORREF editBg = RGB(
                std::min(255, GetRValue(colors_.menuBg) + 18),
                std::min(255, GetGValue(colors_.menuBg) + 18),
                std::min(255, GetBValue(colors_.menuBg) + 18));
            SetBkColor(hdcEdit, editBg);
            if (!editBgBrush_) editBgBrush_ = CreateSolidBrush(editBg);
            return reinterpret_cast<LRESULT>(editBgBrush_);
        }
        break;

    case WM_KILLFOCUS:
        // Suppress if focus moved to our active child submenu.
        if (subMenuHwnd_ && (HWND)wParam == subMenuHwnd_) return 0;
        // Suppress if focus moved to our search EDIT control.
        if (searchEdit_ && (HWND)wParam == searchEdit_) return 0;
        // Suppress while power popup is open.
        if (suppressKillFocus_) return 0;
        done_ = true;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        done_ = true;
        // Clean up the EDIT control's font and background brush
        if (searchEdit_) {
            HFONT editFont = reinterpret_cast<HFONT>(
                SendMessageW(searchEdit_, WM_GETFONT, 0, 0));
            if (editFont) DeleteObject(editFont);
            searchEdit_ = nullptr;
        }
        if (editBgBrush_) { DeleteObject(editBgBrush_); editBgBrush_ = nullptr; }
        if (folderIconList_) { DestroyIcon(folderIconList_); folderIconList_ = nullptr; }
        if (folderIconGrid_) { DestroyIcon(folderIconGrid_); folderIconGrid_ = nullptr; }
        if (settingsIcon_)   { DestroyIcon(settingsIcon_);   settingsIcon_   = nullptr; }
        for (auto& [path, icon] : exeIconCache_)
            if (icon) DestroyIcon(icon);
        exeIconCache_.clear();
        return 0;
    default: break;
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

    // Search: only on root menu when enabled in settings
    menu.searchEnabled_ = !isSubmenu && settings.appMenuSearchEnabled;
    menu.searchBoxH_    = menu.SearchBoxHeight();

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

    int maxH = 0;
    if (settings.appMenuLayout == AppMenuLayout::Grid &&
        !menu.entryRects_.empty() && settings.appMenuGridCols > 0)
    {
        int cols  = std::max(1, settings.appMenuGridCols);
        int cellH = std::cmp_greater_equal(menu.entryRects_.size(), cols)
                        ? menu.entryRects_[cols - 1].bottom - padPx
                        : menu.entryRects_.back().bottom    - padPx;
        int maxRows = std::max(1, settings.appMenuGridRows);
        maxH = std::min(Scale(settings.appMenuMaxHeight, dpi), 2 * padPx + cellH * maxRows);
    } else {
        maxH = Scale(settings.appMenuMaxHeight, dpi);
    }

    // Add search box height to total menu height
    int menuH = std::min(contentH + menu.searchBoxH_, maxH + menu.searchBoxH_);
    menu.menuH_ = menuH;
    menu.UpdateMaxScroll(contentH, menuH - menu.searchBoxH_);

    HMONITOR hMon = MonitorFromRect(&anchorRect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfo(hMon, &mi);
    RECT workArea = mi.rcWork;

    int x = 0, y = 0;
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
            msg.hwnd != hwnd && msg.hwnd != menu.searchEdit_)
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
