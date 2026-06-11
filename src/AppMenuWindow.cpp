#include "AppMenuWindow.h"
#include "LaunchHelper.h"
#include "SettingsSearch.h"
#include "SystemTools.h"
#include "PopupMenu.h"
#include "Dpi.h"
#include "resource.h"
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <powrprof.h>
#include <wincodec.h>
#include <dwmapi.h>
#include <algorithm>
#include <unordered_set>
#include <utility>

#pragma comment(lib, "PowrProf.lib")
#pragma comment(lib, "windowscodecs.lib")

static constexpr wchar_t kAppMenuClass[] = L"WinzooAppMenu";
// Deferred message to show the Classic power submenu outside of a WM_LBUTTONUP handler,
// avoiding TrackPopupMenu dismissal bugs when called from within button-up processing.
static constexpr UINT WM_CLASSIC_POWER = WM_APP + 1;
// Posted to the root menu when the pin list changes from a submenu, so the root's pinned
// section refreshes live (the root's own message loop is paused while a submenu is open).
static constexpr UINT WM_PINS_CHANGED  = WM_APP + 2;

// Helper to launch an app with optional same-monitor hint. Always routes through
// LaunchOnMonitor (passing a null monitor when no hint) so the launched window is
// tracked and brought to the foreground — without this, a window launched via
// Explorer's de-elevating automation object can open unfocused. The monitor hint
// only adds the move-to-taskbar-monitor step on top.
static void LaunchMenuApp(HWND hwndHint, bool useHint, const wchar_t* exe, const wchar_t* args = nullptr, int nShow = SW_SHOWNORMAL)
{
    HMONITOR hMon = (useHint && hwndHint)
        ? MonitorFromWindow(hwndHint, MONITOR_DEFAULTTONEAREST)
        : nullptr;
    LaunchOnMonitor(GetModuleHandleW(nullptr), hMon, exe, args, nShow);
}

// ---------- cached power options ----------

static std::vector<PowerOption> s_powerOptions;
static void ExecutePowerAction(PowerOption::Action action);

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

void AppMenuWindow::ShowPowerSubmenu(HWND hwndOwner, POINT ptScreen)
{
    if (s_powerOptions.empty()) return;

    HMENU hMenu = CreatePopupMenu();
    for (int i = 0; std::cmp_less(i, s_powerOptions.size()); ++i)
        AppendMenuW(hMenu, MF_STRING, i + 1, s_powerOptions[i].label.c_str());

    int cmd = static_cast<int>(TrackPopupMenu(hMenu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN,
        ptScreen.x, ptScreen.y, 0, hwndOwner, nullptr));
    DestroyMenu(hMenu);

    if (cmd > 0)
        ExecutePowerAction(s_powerOptions[cmd - 1].action);
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

// ---------- user-pinned apps helpers ----------

// Returns the active pinned-path list (global or per-monitor) for reading.
static const std::vector<std::wstring>& ActivePins(const Settings& s, const std::wstring& monitorName)
{
    static const std::vector<std::wstring> kEmpty;
    if (s.appMenuPinnedPerMonitor && !monitorName.empty()) {
        auto it = s.appMenuPinnedPathsPerMonitor.find(monitorName);
        return (it != s.appMenuPinnedPathsPerMonitor.end()) ? it->second : kEmpty;
    }
    return s.appMenuPinnedPaths;
}

// Returns the active pinned-path list for mutation (creates the per-monitor entry if needed).
static std::vector<std::wstring>& MutableActivePins(Settings& s, const std::wstring& monitorName)
{
    if (s.appMenuPinnedPerMonitor && !monitorName.empty())
        return s.appMenuPinnedPathsPerMonitor[monitorName];
    return s.appMenuPinnedPaths;
}

// Builds the root node list: pinned leaves first (stored order, isPinned=true), then the
// normal app tree (with flatten applied). outPinnedCount receives the number of pinned leaves.
static std::vector<AppTreeNode> BuildRootNodes(const std::vector<AppEntry>& entries,
                                               const Settings& settings,
                                               const std::wstring& monitorName,
                                               int& outPinnedCount)
{
    std::vector<AppTreeNode> tree = BuildAppTree(entries);

    if (settings.appMenuFlattenMode == AppMenuFlattenMode::All) {
        std::vector<AppTreeNode> flat;
        FlattenTreeInto(tree, flat);
        std::sort(flat.begin(), flat.end(), [](const AppTreeNode& a, const AppTreeNode& b) {
            return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
        });
        tree = std::move(flat);
    }

    // Resolve pinned paths to leaf nodes via the entry snapshot (for name + icon).
    const std::vector<std::wstring>& pins = ActivePins(settings, monitorName);
    std::vector<AppTreeNode> pinnedNodes;
    pinnedNodes.reserve(pins.size());
    for (const auto& p : pins) {
        const AppEntry* match = nullptr;
        for (const auto& e : entries) {
            if (_wcsicmp(e.exePath.c_str(),  p.c_str()) == 0 ||
                _wcsicmp(e.iconPath.c_str(), p.c_str()) == 0) { match = &e; break; }
        }
        if (!match) continue;  // uninstalled / not found — skip silently
        AppTreeNode leaf;
        leaf.name     = match->name;
        leaf.isFolder = false;
        leaf.icon     = match->icon;
        leaf.exePath  = match->exePath;
        leaf.iconPath = match->iconPath;
        leaf.isPinned = true;
        pinnedNodes.push_back(std::move(leaf));
    }

    outPinnedCount = static_cast<int>(pinnedNodes.size());

    if (!pinnedNodes.empty()) {
        pinnedNodes.insert(pinnedNodes.end(),
                           std::make_move_iterator(tree.begin()),
                           std::make_move_iterator(tree.end()));
        return pinnedNodes;
    }
    return tree;
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

// ---------- Classic (Vista/7) layout helpers ----------

struct ClassicLink {
    int            id;
    const wchar_t* label;
    const wchar_t* glyph;   // Segoe MDL2 Assets codepoint
    bool           chevron; // draw ">" on the right
};

// Returns the enabled right-panel links (excluding Shut Down, which is pinned to the bottom).
static std::vector<ClassicLink> BuildClassicLinks(const Settings& s)
{
    std::vector<ClassicLink> links;
    if (s.appMenuClassicShowDocuments)    links.push_back({ 0, L"Documents",    L"", false });
    if (s.appMenuClassicShowPictures)     links.push_back({ 1, L"Pictures",     L"", false });
    if (s.appMenuClassicShowMusic)        links.push_back({ 2, L"Music",        L"", false });
    if (s.appMenuClassicShowDownloads)    links.push_back({ 3, L"Downloads",    L"", false });
    if (s.appMenuClassicShowRecentItems)  links.push_back({ 4, L"Recent Items", L"", true  });
    if (s.appMenuClassicShowThisPC)       links.push_back({ 5, L"This PC",      L"", false });
    if (s.appMenuClassicShowControlPanel) links.push_back({ 6, L"Control Panel",L"", false });
    if (s.appMenuClassicShowWinSettings)  links.push_back({ 7, L"Settings",     L"", false });
    if (s.appMenuClassicShowRun)          links.push_back({ 8, L"Run...",       L"", false });
    return links;
}

// Load the Windows account profile picture at targetSize x targetSize.
// Returns an HBITMAP on success; nullptr on failure (caller must DeleteObject when done).
static HBITMAP LoadUserProfilePicture(int targetSize)
{
    wchar_t path[MAX_PATH * 2] = {};
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\AccountPicture",
        0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return nullptr;

    for (auto* val : { L"Image448", L"Image240", L"Image96", L"Image64" }) {
        DWORD sz = sizeof(path);
        if (RegQueryValueExW(hKey, val, nullptr, nullptr,
            reinterpret_cast<BYTE*>(path), &sz) == ERROR_SUCCESS && path[0])
            break;
        path[0] = 0;
    }
    RegCloseKey(hKey);
    if (!path[0]) return nullptr;

    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory))))
        return nullptr;

    IWICBitmapDecoder* decoder = nullptr;
    if (FAILED(factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder))) {
        factory->Release(); return nullptr;
    }

    IWICBitmapFrameDecode* frame = nullptr;
    if (FAILED(decoder->GetFrame(0, &frame))) {
        decoder->Release(); factory->Release(); return nullptr;
    }
    decoder->Release();

    IWICBitmapScaler* scaler = nullptr;
    if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(frame, static_cast<UINT>(targetSize),
            static_cast<UINT>(targetSize), WICBitmapInterpolationModeFant))) {
        if (scaler) scaler->Release();
        frame->Release(); factory->Release(); return nullptr;
    }
    frame->Release();

    IWICFormatConverter* conv = nullptr;
    if (FAILED(factory->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(scaler, GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeMedianCut))) {
        if (conv) conv->Release();
        scaler->Release(); factory->Release(); return nullptr;
    }
    scaler->Release();

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth       = targetSize;
    bmi.bmiHeader.biHeight      = -targetSize;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hBmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (hBmp && bits) {
        UINT stride = static_cast<UINT>(targetSize * 4);
        if (FAILED(conv->CopyPixels(nullptr, stride, stride * targetSize,
            static_cast<BYTE*>(bits)))) {
            DeleteObject(hBmp); hBmp = nullptr;
        }
    }
    conv->Release();
    factory->Release();
    return hBmp;
}

// Draw hBmp (already targetSize x targetSize) clipped to a circle at (x, y).
static void DrawCircularBitmap(HDC hdc, HBITMAP hBmp, int x, int y, int size)
{
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(memDC, hBmp));
    HRGN clipRgn = CreateEllipticRgn(x, y, x + size, y + size);
    SelectClipRgn(hdc, clipRgn);
    BitBlt(hdc, x, y, size, size, memDC, 0, 0, SRCCOPY);
    SelectClipRgn(hdc, nullptr);
    DeleteObject(clipRgn);
    SelectObject(memDC, oldBmp);
    DeleteDC(memDC);
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
    bool isList = (settings_->appMenuLayout == AppMenuLayout::List ||
                   settings_->appMenuLayout == AppMenuLayout::Classic ||
                   settings_->appMenuLayout == AppMenuLayout::ClassicRounded);

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
        // When a pinned section is shown, force the rest of the apps onto a fresh row so
        // pinned and non-pinned items never share a row.
        bool breakAfterPinned = (nodes_ == &ownedNodes_) && pinnedCount_ > 0 &&
                                std::cmp_less(pinnedCount_, nodes_->size());
        int row = 0, col = 0;
        for (size_t i = 0; i < nodes_->size(); ++i) {
            if (breakAfterPinned && std::cmp_equal(i, pinnedCount_) && col != 0) {
                col = 0; ++row;  // start the non-pinned apps on a new row
            }
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

            // System tools (Device Manager, Disk Management, Control Panel
            // applets, …) — append below shortcut matches. Source is dynamic
            // shell enumeration or the curated table per settings.
            auto tools = SearchSystemTools(lowerQuery, settings_->appMenuSearchSystemDynamic,
                                           fuzzy);
            if (!tools.empty() && !sysToolIcon_) {
                wchar_t controlExe[MAX_PATH];
                ExpandEnvironmentStringsW(L"%windir%\\System32\\control.exe",
                                          controlExe, MAX_PATH);
                ExtractIconExW(controlExe, 0, nullptr, &sysToolIcon_, 1);
                if (!sysToolIcon_)
                    sysToolIcon_ = LoadIcon(nullptr, IDI_APPLICATION);
            }
            // Dedup against names already shown (shortcuts + settings pages) so
            // e.g. "Control Panel" doesn't appear twice from different sources.
            std::unordered_set<std::wstring> seen;
            for (const auto& n : filteredNodes_) {
                std::wstring key = n.name;
                for (auto& ch : key) ch = towlower(ch);
                seen.insert(std::move(key));
            }
            for (auto& t : tools) {
                std::wstring key = t.name;
                for (auto& ch : key) ch = towlower(ch);
                if (!seen.insert(key).second) continue;  // already shown

                AppTreeNode n;
                n.name     = std::move(t.name);
                n.subtitle = L"System tool";
                n.icon     = sysToolIcon_;
                if (t.shellItem) {
                    n.type = AppNodeType::ShellItem;
                } else {
                    n.type    = AppNodeType::Executable;
                    n.exePath = std::move(t.command);
                }
                filteredNodes_.push_back(std::move(n));
            }

            // Executable from PATH — append below shortcut matches.
            // Guard: searchText_ could exceed MAX_PATH; PathFindOnPathW requires MAX_PATH buffer.
            if (searchText_.size() < MAX_PATH - 5) {  // room for ".exe\0"
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
        }

        nodes_ = &filteredNodes_;
    }
    BuildEntryRects(menuW_);
    int padPx    = Scale(settings_ ? settings_->appMenuPadding : 6, dpi_);
    int contentH = ContentHeight(entryRects_) + padPx;
    int clientH  = menuH_ - searchBoxH_ - classicFooterH_;
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
        case VK_PRIOR:
        case VK_NEXT:
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

    bool isClassic = (settings_->appMenuLayout == AppMenuLayout::Classic ||
                      settings_->appMenuLayout == AppMenuLayout::ClassicRounded);
    bool isRounded = (settings_->appMenuLayout == AppMenuLayout::ClassicRounded);
    bool isList    = (settings_->appMenuLayout == AppMenuLayout::List || isClassic);

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
    int contentAreaH = h - searchBoxH_ - classicFooterH_;  // area available for entries (above search box and classic footer)

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

    // Divider between the user-pinned section and the rest of the apps.
    if (pinnedCount_ > 0 && nodes_ == &ownedNodes_ &&
        std::cmp_less(pinnedCount_, nodes_->size()) &&
        std::cmp_less_equal(pinnedCount_, entryRects_.size()))
    {
        int sepY = entryRects_[pinnedCount_ - 1].bottom - scrollOffset_;
        if (sepY > 0 && sepY < contentAreaH) {
            RECT sepR = { pad, sepY, menuW_ - pad, sepY + Scale(1, dpi_) };
            HBRUSH sepBr = CreateSolidBrush(colors_.separator);
            FillRect(hdc, &sepR, sepBr);
            DeleteObject(sepBr);
        }
    }

    // Drag-reorder insertion indicator.
    if (drag_.State() == DragState::Dragging && dropIndex_ >= 0 && pinnedCount_ > 0) {
        HBRUSH insBr = CreateSolidBrush(colors_.menuText);
        int thick = Scale(2, dpi_);
        if (isList) {
            int y = (dropIndex_ <= 0)
                        ? entryRects_[0].top
                        : entryRects_[std::min(dropIndex_, pinnedCount_) - 1].bottom;
            y -= scrollOffset_;
            RECT insR = { pad, y - thick / 2, menuW_ - pad, y + thick - thick / 2 };
            FillRect(hdc, &insR, insBr);
        } else {
            int x, top, bot;
            if (dropIndex_ < pinnedCount_) {
                const RECT& r = entryRects_[dropIndex_];
                x = r.left; top = r.top; bot = r.bottom;
            } else {
                const RECT& r = entryRects_[pinnedCount_ - 1];
                x = r.right; top = r.top; bot = r.bottom;
            }
            top -= scrollOffset_; bot -= scrollOffset_;
            RECT insR = { x - thick / 2, top, x + thick - thick / 2, bot };
            FillRect(hdc, &insR, insBr);
        }
        DeleteObject(insBr);
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

    // Classic footer: "> All Programs" / "< Back" above the search box
    if (isClassic && classicFooterH_ > 0) {
        int footerY = h - searchBoxH_ - classicFooterH_;
        int sepInset = isRounded ? Scale(8, dpi_) : 0;
        RECT sepR = { sepInset, footerY, menuW_ - sepInset, footerY + 1 };
        HBRUSH sepBr = CreateSolidBrush(colors_.separator);
        FillRect(hdc, &sepR, sepBr);
        DeleteObject(sepBr);

        RECT footerR = { 0, footerY + 1, menuW_, footerY + classicFooterH_ };
        if (classicFooterHovered_) {
            HBRUSH hb = CreateSolidBrush(colors_.menuHover);
            FillRect(hdc, &footerR, hb);
            DeleteObject(hb);
        }
        RECT textR = footerR;
        textR.left  += Scale(8, dpi_);
        textR.right -= Scale(4, dpi_);
        SetTextColor(hdc, colors_.menuText);
        const wchar_t* footerText = inAllPrograms_ ? L"◄ Back" : L"► All Programs";
        DrawTextW(hdc, footerText, -1, &textR, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    }

    // Search box at the bottom of the content area
    if (searchBoxH_ > 0) {
        int sbY     = h - searchBoxH_;
        // ClassicRounded keeps search in the left panel only; plain Classic spans full width
        int sbWidth = (isClassic && !isRounded) ? w : menuW_;

        if (isRounded) {
            // Fill the search strip with menu background first
            RECT bgR = { 0, sbY, sbWidth, h };
            HBRUSH bgBr = CreateSolidBrush(colors_.menuBg);
            FillRect(hdc, &bgR, bgBr);
            DeleteObject(bgBr);

            // Rounded input box
            int bInset = Scale(8, dpi_);   // horizontal margin from window edge
            int bVInset = Scale(3, dpi_);  // vertical margin
            COLORREF inputBg = RGB(
                std::min(255, GetRValue(colors_.menuBg) + 18),
                std::min(255, GetGValue(colors_.menuBg) + 18),
                std::min(255, GetBValue(colors_.menuBg) + 18));
            HBRUSH inputBrush = CreateSolidBrush(inputBg);
            HPEN   inputPen   = CreatePen(PS_SOLID, 1, colors_.separator);
            HBRUSH oldBr  = static_cast<HBRUSH>(SelectObject(hdc, inputBrush));
            HPEN   oldPen = static_cast<HPEN>(SelectObject(hdc, inputPen));
            int rx = Scale(8, dpi_); // ellipse diameter (\u2248 4 px radius)
            RoundRect(hdc, bInset, sbY + bVInset, sbWidth - bInset, h - bVInset, rx, rx);
            SelectObject(hdc, oldBr);
            SelectObject(hdc, oldPen);
            DeleteObject(inputBrush);
            DeleteObject(inputPen);

            // Magnifying glass icon inside the box
            int iconPad   = Scale(6, dpi_);
            int iconAreaW = Scale(16, dpi_);
            LOGFONTW lfIcon = {};
            lfIcon.lfHeight  = -Scale(12, dpi_);
            lfIcon.lfQuality = CLEARTYPE_QUALITY;
            wcscpy_s(lfIcon.lfFaceName, L"Segoe MDL2 Assets");
            HFONT iconFont  = CreateFontIndirectW(&lfIcon);
            HFONT prevFont2 = static_cast<HFONT>(SelectObject(hdc, iconFont));
            SetTextColor(hdc, colors_.textDimmed);
            RECT iconR = { bInset + iconPad, sbY + bVInset,
                           bInset + iconPad + iconAreaW, h - bVInset };
            DrawTextW(hdc, L"\uE721", 1, &iconR, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
            SelectObject(hdc, prevFont2);
            DeleteObject(iconFont);
        } else {
            int sbPad = Scale(4, dpi_);

            // Separator line above search box
            RECT sepLine = { 0, sbY, sbWidth, sbY + 1 };
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
        BtnDef btns[7];
        int btnCount = 0;
        btns[btnCount++] = { 3, L"\uE946" };  // Winzoo Settings
        btns[btnCount++] = { 4, L"\uE8F1" };  // All Apps
        btns[btnCount++] = { 5, L"\uE768" };  // Run dialog
        btns[btnCount++] = { 6, L"\uE756" };  // Windows Terminal
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

    // Classic right panel
    if (isClassic && classicPanelW_ > 0 && settings_) {
        int rxL = menuW_;
        int rxR = w;
        int linkH   = Scale(settings_->appMenuEntryHeight, dpi_);
        int picSize = Scale(48, dpi_);
        int innerPad = Scale(6, dpi_);
        // ClassicRounded: search box stays in the left panel, so the right panel fills to the bottom
        int panelBottom = (isRounded) ? h : h - searchBoxH_;

        // Right panel background (slightly different shade)
        COLORREF rightBg = RGB(
            std::max(0, GetRValue(colors_.menuBg) - 10),
            std::max(0, GetGValue(colors_.menuBg) - 10),
            std::max(0, GetBValue(colors_.menuBg) - 10));
        RECT rightR = { rxL, 0, rxR, h };
        HBRUSH rightBr = CreateSolidBrush(rightBg);
        FillRect(hdc, &rightR, rightBr);
        DeleteObject(rightBr);

        // Left separator line
        RECT sepR2 = { rxL, 0, rxL + Scale(1, dpi_), h };
        HBRUSH sep2Br = CreateSolidBrush(colors_.separator);
        FillRect(hdc, &sepR2, sep2Br);
        DeleteObject(sep2Br);

        // --- Profile area ---
        int picY = Scale(8, dpi_);
        int picX = rxL + (classicPanelW_ - picSize) / 2;
        int profileH = picY + picSize + Scale(20, dpi_);  // top + pic + name area

        if (profilePicBmp_) {
            DrawCircularBitmap(hdc, profilePicBmp_, picX, picY, picSize);
        } else {
            // Fallback: draw person glyph inside a circle
            HRGN circleRgn = CreateEllipticRgn(picX, picY, picX + picSize, picY + picSize);
            HBRUSH circleBr = CreateSolidBrush(colors_.buttonNormal);
            FillRgn(hdc, circleRgn, circleBr);
            DeleteObject(circleBr);
            DeleteObject(circleRgn);

            LOGFONTW lfPerson = {};
            lfPerson.lfHeight  = -(picSize * 2 / 3);
            lfPerson.lfQuality = CLEARTYPE_QUALITY;
            wcscpy_s(lfPerson.lfFaceName, L"Segoe MDL2 Assets");
            HFONT personFont = CreateFontIndirectW(&lfPerson);
            HFONT prevPF     = static_cast<HFONT>(SelectObject(hdc, personFont));
            RECT personR = { picX, picY, picX + picSize, picY + picSize };
            SetTextColor(hdc, colors_.textDimmed);
            DrawTextW(hdc, L"", 1, &personR, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
            SelectObject(hdc, prevPF);
            DeleteObject(personFont);
        }

        // Username
        wchar_t userName[256] = L"User";
        DWORD unLen = static_cast<DWORD>(std::size(userName));
        GetUserNameW(userName, &unLen);
        RECT nameR = { rxL + innerPad, picY + picSize + Scale(2, dpi_),
                       rxR - innerPad, profileH };
        SetTextColor(hdc, colors_.menuText);
        SelectObject(hdc, font);
        DrawTextW(hdc, userName, -1, &nameR,
                  DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

        // --- Links ---
        auto links = BuildClassicLinks(*settings_);
        bool hasShutdown = settings_->appMenuClassicShowShutDown;

        int shutdownY    = panelBottom - linkH;
        int linksBottom  = hasShutdown ? shutdownY - Scale(1, dpi_) : panelBottom;

        // MDL2 font for link icons
        LOGFONTW lfMdl = {};
        lfMdl.lfHeight  = -Scale(14, dpi_);
        lfMdl.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lfMdl.lfFaceName, L"Segoe MDL2 Assets");
        HFONT mdlFont  = CreateFontIndirectW(&lfMdl);

        int iconAreaW2 = Scale(22, dpi_);

        for (int li = 0; li < static_cast<int>(links.size()); ++li) {
            int ly = profileH + li * linkH;
            if (ly + linkH > linksBottom) break;

            RECT linkR = { rxL + Scale(1, dpi_), ly, rxR, ly + linkH };
            if (li == classicHoveredRight_) {
                HBRUSH hb = CreateSolidBrush(colors_.menuHover);
                FillRect(hdc, &linkR, hb);
                DeleteObject(hb);
            }
            // Icon
            HFONT prevMdl = static_cast<HFONT>(SelectObject(hdc, mdlFont));
            RECT iconR2 = { rxL + innerPad, ly, rxL + innerPad + iconAreaW2, ly + linkH };
            SetTextColor(hdc, colors_.menuText);
            DrawTextW(hdc, links[li].glyph, 1, &iconR2,
                      DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
            SelectObject(hdc, prevMdl);

            // Label
            SelectObject(hdc, font);
            int chevW2 = links[li].chevron ? Scale(14, dpi_) : 0;
            RECT lblR = { rxL + innerPad + iconAreaW2 + Scale(4, dpi_), ly,
                          rxR - innerPad - chevW2, ly + linkH };
            SetTextColor(hdc, colors_.menuText);
            DrawTextW(hdc, links[li].label, -1, &lblR,
                      DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
            if (links[li].chevron) {
                RECT chevR2 = { rxR - innerPad - chevW2, ly, rxR - innerPad, ly + linkH };
                SetTextColor(hdc, colors_.textDimmed);
                DrawTextW(hdc, L"▶", 1, &chevR2, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
            }
        }

        // Separator before shutdown
        if (hasShutdown) {
            RECT sdSep = { rxL + Scale(1, dpi_), shutdownY - Scale(1, dpi_), rxR, shutdownY };
            HBRUSH sdSepBr = CreateSolidBrush(colors_.separator);
            FillRect(hdc, &sdSep, sdSepBr);
            DeleteObject(sdSepBr);

            int sdIdx = static_cast<int>(links.size());
            RECT sdR = { rxL + Scale(1, dpi_), shutdownY, rxR, shutdownY + linkH };
            if (classicHoveredRight_ == sdIdx) {
                HBRUSH hb = CreateSolidBrush(colors_.menuHover);
                FillRect(hdc, &sdR, hb);
                DeleteObject(hb);
            }
            // Icon
            HFONT prevMdl2 = static_cast<HFONT>(SelectObject(hdc, mdlFont));
            RECT sdIconR = { rxL + innerPad, shutdownY, rxL + innerPad + iconAreaW2, shutdownY + linkH };
            SetTextColor(hdc, colors_.menuText);
            DrawTextW(hdc, L"", 1, &sdIconR,
                      DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
            SelectObject(hdc, prevMdl2);

            // Label + chevron
            SelectObject(hdc, font);
            int chevW3 = Scale(14, dpi_);
            RECT sdLblR = { rxL + innerPad + iconAreaW2 + Scale(4, dpi_), shutdownY,
                            rxR - innerPad - chevW3, shutdownY + linkH };
            SetTextColor(hdc, colors_.menuText);
            DrawTextW(hdc, L"Shut down", -1, &sdLblR,
                      DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
            RECT sdChevR = { rxR - innerPad - chevW3, shutdownY, rxR - innerPad, shutdownY + linkH };
            SetTextColor(hdc, colors_.textDimmed);
            DrawTextW(hdc, L"▶", 1, &sdChevR, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        }

        DeleteObject(mdlFont);
    }

    SelectObject(hdc, oldFont);
    DeleteObject(font);
}

// ---------- hit testing ----------

int AppMenuWindow::HitTestEntry(POINT ptClient) const
{
    if (ptClient.x >= menuW_) return -1;  // in sidebar / right panel
    if (searchBoxH_ > 0 && ptClient.y >= menuH_ - searchBoxH_) return -1;  // in search box
    // In Classic mode, the footer row (All Programs/Back) is not an entry
    if (classicFooterH_ > 0 && ptClient.y >= menuH_ - searchBoxH_ - classicFooterH_) return -1;
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
    int enabled[7];
    int enabledCount = 0;
    enabled[enabledCount++] = 3;  // Winzoo Settings (always visible)
    enabled[enabledCount++] = 4;  // All Apps (always visible)
    enabled[enabledCount++] = 5;  // Run dialog (always visible)
    enabled[enabledCount++] = 6;  // Windows Terminal (always visible)
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

// ---------- Classic right-panel hit testing ----------

int AppMenuWindow::HitTestClassicRight(POINT ptClient) const
{
    if (classicPanelW_ <= 0 || !settings_) return -1;
    if (ptClient.x < menuW_) return -1;
    // For ClassicRounded the search box is left-panel only; right panel reaches the full height.
    bool rightHasSearch = (settings_->appMenuLayout != AppMenuLayout::ClassicRounded);
    int  panelBottom    = (rightHasSearch && searchBoxH_ > 0) ? menuH_ - searchBoxH_ : menuH_;
    if (ptClient.y >= panelBottom) return -1;

    int linkH    = Scale(settings_->appMenuEntryHeight, dpi_);
    int picSize  = Scale(48, dpi_);
    int pad      = Scale(6, dpi_);
    int profileH = picSize + Scale(24, dpi_);  // picture + name line + padding

    auto links = BuildClassicLinks(*settings_);
    bool hasShutdown = settings_->appMenuClassicShowShutDown;

    // Shutdown row sits at the very bottom of the right panel
    int shutdownY = panelBottom - linkH;
    if (hasShutdown && ptClient.y >= shutdownY && ptClient.y < panelBottom)
        return static_cast<int>(links.size());  // shutdown index

    // Links area below profile header
    if (ptClient.y < profileH) return -1;  // in profile area, not clickable
    int relY  = ptClient.y - profileH;
    int idx   = relY / linkH;
    int maxLinks = static_cast<int>(links.size());
    int linksAreaBottom = hasShutdown ? shutdownY - Scale(1, dpi_) : panelBottom;
    if (ptClient.y >= linksAreaBottom) return -1;
    if (idx >= 0 && idx < maxLinks) return idx;
    (void)pad;
    return -1;
}

bool AppMenuWindow::HitTestClassicFooter(POINT ptClient) const
{
    if (classicFooterH_ <= 0) return false;
    if (ptClient.x >= menuW_) return false;
    int footerY = menuH_ - searchBoxH_ - classicFooterH_;
    return ptClient.y >= footerY && ptClient.y < footerY + classicFooterH_;
}

// ---------- Classic right-panel activation ----------

void AppMenuWindow::ActivateClassicRight(int linkIdx)
{
    if (!settings_) return;
    auto links = BuildClassicLinks(*settings_);
    bool hasShutdown = settings_->appMenuClassicShowShutDown;
    int  shutdownIdx = static_cast<int>(links.size());

    if (linkIdx == shutdownIdx && hasShutdown) {
        // Power submenu
        if (s_powerOptions.empty()) return;
        HMENU hMenu = CreatePopupMenu();
        for (int i = 0; std::cmp_less(i, s_powerOptions.size()); ++i)
            AppendMenuW(hMenu, MF_STRING, i + 1, s_powerOptions[i].label.c_str());
        // Anchor the popup at the top-right of the shutdown row (grows left+up from there).
        // For ClassicRounded the right panel has no search box, so the row sits at full bottom.
        bool rightHasSearch2 = (settings_->appMenuLayout != AppMenuLayout::ClassicRounded);
        int  sdPanelBottom   = (rightHasSearch2 && searchBoxH_ > 0) ? menuH_ - searchBoxH_ : menuH_;
        POINT btnPt = { menuW_ + classicPanelW_,
                        sdPanelBottom - Scale(settings_->appMenuEntryHeight, dpi_) };
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
            if (IsWindow(hwnd_)) SetForegroundWindow(hwnd_);
        }
        return;
    }
    if (linkIdx < 0 || linkIdx >= static_cast<int>(links.size())) return;

    bool sameMonitor = settings_->openAppsOnSameMonitor;
    auto launch = [&](const wchar_t* path, const wchar_t* args = nullptr) {
        LaunchMenuApp(hwnd_, sameMonitor, path, args);
        closeReason_ = AppMenuCloseReason::Selection;
        done_ = true;
        DestroyWindow(hwnd_);
    };

    switch (links[linkIdx].id) {
    case 0: launch(L"explorer.exe", L"shell:Personal");      break;  // Documents
    case 1: launch(L"explorer.exe", L"shell:My Pictures");   break;  // Pictures
    case 2: launch(L"explorer.exe", L"shell:My Music");      break;  // Music
    case 3: launch(L"explorer.exe", L"shell:Downloads");     break;
    case 4: launch(L"explorer.exe", L"shell:Recent");        break;  // Recent Items
    case 5: launch(L"explorer.exe", L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}"); break;  // This PC
    case 6: launch(L"explorer.exe", L"shell:::{26EE0668-A00A-44D7-9371-BEB064C98683}"); break;  // Control Panel
    case 7: launch(L"ms-settings:");       break;
    case 8:
        ShowRunDialog(hwnd_);
        closeReason_ = AppMenuCloseReason::Selection;
        done_ = true;
        DestroyWindow(hwnd_);
        break;
    default: break;
    }
}

// ---------- Classic All Programs toggle ----------

void AppMenuWindow::ToggleAllPrograms()
{
    if (!settings_) return;
    if (!inAllPrograms_) {
        // Build flat sorted all-programs list if not done yet
        if (allProgramsNodes_.empty()) {
            FlattenTreeInto(ownedNodes_, allProgramsNodes_);
            std::sort(allProgramsNodes_.begin(), allProgramsNodes_.end(),
                [](const AppTreeNode& a, const AppTreeNode& b) {
                    return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
                });
        }
        nodes_         = &allProgramsNodes_;
        inAllPrograms_ = true;
    } else {
        nodes_         = &ownedNodes_;
        inAllPrograms_ = false;
    }
    scrollOffset_ = 0;
    hoveredIdx_   = -1;
    BuildEntryRects(menuW_);
    int padPx    = Scale(settings_->appMenuPadding, dpi_);
    int contentH = ContentHeight(entryRects_) + padPx;
    UpdateMaxScroll(contentH, menuH_ - searchBoxH_ - classicFooterH_);
    InvalidateRect(hwnd_, nullptr, FALSE);
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
            &subMenuHwnd_,  // parent stores child HWND for WM_KILLFOCUS guard
            /*pinnedCount=*/0, /*entries=*/{},
            monitorDeviceName_, onSettingsChanged_, pinSettings_, rootHwnd_);  // inherit pin context
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
    } else if (node.type == AppNodeType::ShellItem) {
        // Dynamic system tool: launch by display name via the shell namespace
        // (de-elevated when possible). No same-monitor hint — these open their
        // own MMC/Control Panel hosts.
        LaunchSystemToolByName(node.name);
        closeReason_ = AppMenuCloseReason::Selection;
        done_ = true;
        DestroyWindow(hwnd_);
    } else {
        const std::wstring& path = node.exePath.empty() ? node.iconPath : node.exePath;
        if (!path.empty())
            LaunchMenuApp(hwnd_, settings_->openAppsOnSameMonitor, path.c_str());
        closeReason_ = AppMenuCloseReason::Selection;
        done_ = true;
        DestroyWindow(hwnd_);
    }
}

// ---------- user-pinned apps: rebuild, context menu, drag ----------

void AppMenuWindow::RebuildRootNodes()
{
    const Settings& src = pinSettings_ ? *pinSettings_ : ownedSettings_;
    ownedNodes_ = BuildRootNodes(ownedEntries_, src, monitorDeviceName_, pinnedCount_);
    nodes_ = &ownedNodes_;
    inAllPrograms_ = false;
    allProgramsNodes_.clear();
    // Clear any active search so the rebuilt list (with the pinned section) is shown.
    if (searchEdit_) SetWindowTextW(searchEdit_, L"");
    searchText_.clear();

    scrollOffset_ = 0;
    hoveredIdx_   = -1;
    BuildEntryRects(menuW_);
    int padPx    = Scale(settings_ ? settings_->appMenuPadding : 6, dpi_);
    int contentH = ContentHeight(entryRects_) + padPx;
    UpdateMaxScroll(contentH, menuH_ - searchBoxH_ - classicFooterH_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppMenuWindow::OnPinsMutated()
{
    // Persist + propagate using the shared settings object.
    if (onSettingsChanged_ && pinSettings_) onSettingsChanged_(*pinSettings_);
    // The pinned section only exists at the root level.
    if (!isSubmenu_) {
        RebuildRootNodes();
    } else if (rootHwnd_ && IsWindow(rootHwnd_)) {
        // Pinned from a submenu: ask the (paused) root menu to refresh live.
        PostMessageW(rootHwnd_, WM_PINS_CHANGED, 0, 0);
    }
}

void AppMenuWindow::ShowEntryContextMenu(int idx, POINT ptScreen)
{
    if (!nodes_ || idx < 0 || std::cmp_greater_equal(idx, nodes_->size())) return;
    const AppTreeNode& node = (*nodes_)[idx];
    if (node.isFolder) return;
    // Only real Start-Menu shortcuts can be pinned (skip folders, settings pages, PATH exes).
    if (node.type != AppNodeType::Shortcut) return;
    const std::wstring path = node.exePath.empty() ? node.iconPath : node.exePath;
    if (path.empty()) return;

    // Taskbar pinned-folder popup: offer folder actions instead of pinning.
    if (isTaskbarFolder_) {
        std::vector<MenuItem> items;
        items.push_back({ L"Use this icon as the folder cover", IDM_APPMENU_FOLDER_SET_COVER,
                          false, false, false, false });
        items.push_back({ L"Move out of folder", IDM_APPMENU_FOLDER_MOVE_OUT,
                          false, false, false, false });
        suppressKillFocus_ = true;
        UINT cmd = PopupMenu::Show(hwnd_, ptScreen, std::move(items), colors_, dpi_);
        suppressKillFocus_ = false;
        if (cmd == IDM_APPMENU_FOLDER_SET_COVER) {
            if (onSetFolderCover_) onSetFolderCover_(path);  // taskbar rebuilds the folder icon
            // The folder contents are unchanged, so keep the popup open.
            if (IsWindow(hwnd_)) SetForegroundWindow(hwnd_);
        } else if (cmd == IDM_APPMENU_FOLDER_MOVE_OUT) {
            if (onRemoveFromFolder_) onRemoveFromFolder_(path);  // taskbar rebuilds its pins
            closeReason_ = AppMenuCloseReason::Selection;        // close so the change shows
            done_ = true;
            if (IsWindow(hwnd_)) DestroyWindow(hwnd_);
        } else if (IsWindow(hwnd_)) {
            SetForegroundWindow(hwnd_);
        }
        return;
    }

    if (!pinSettings_) return;
    const std::vector<std::wstring>& pins = ActivePins(*pinSettings_, monitorDeviceName_);
    bool isPinned = std::any_of(pins.begin(), pins.end(),
        [&](const std::wstring& p){ return _wcsicmp(p.c_str(), path.c_str()) == 0; });

    std::vector<MenuItem> items;
    items.push_back({ isPinned ? L"Unpin from Apps Menu" : L"Pin to Apps Menu",
                      IDM_APPMENU_PIN_TOGGLE, false, false, false, false });

    suppressKillFocus_ = true;
    UINT cmd = PopupMenu::Show(hwnd_, ptScreen, std::move(items), colors_, dpi_);
    suppressKillFocus_ = false;

    if (cmd == IDM_APPMENU_PIN_TOGGLE) {
        std::vector<std::wstring>& mut = MutableActivePins(*pinSettings_, monitorDeviceName_);
        if (isPinned) {
            mut.erase(std::remove_if(mut.begin(), mut.end(),
                [&](const std::wstring& p){ return _wcsicmp(p.c_str(), path.c_str()) == 0; }),
                mut.end());
        } else {
            mut.push_back(path);
        }
        OnPinsMutated();
    } else if (IsWindow(hwnd_)) {
        SetForegroundWindow(hwnd_);  // dismissed without selection — keep menu active
    }
}

int AppMenuWindow::ComputeDropIndex(POINT ptClient) const
{
    if (pinnedCount_ <= 0) return 0;
    bool isGrid = settings_ && settings_->appMenuLayout == AppMenuLayout::Grid;
    int contentX = ptClient.x;
    int contentY = ptClient.y + scrollOffset_;

    int  nearest  = -1;
    long bestDist = 0;
    for (int i = 0; i < pinnedCount_ && std::cmp_less(i, entryRects_.size()); ++i) {
        const RECT& r = entryRects_[i];
        long midX = (r.left + r.right) / 2;
        long midY = (r.top + r.bottom) / 2;
        long dx = contentX - midX, dy = contentY - midY;
        long dist = isGrid ? (dx * dx + dy * dy) : (dy < 0 ? -dy : dy);
        if (nearest < 0 || dist < bestDist) { bestDist = dist; nearest = i; }
    }
    if (nearest < 0) return pinnedCount_;
    const RECT& r = entryRects_[nearest];
    bool after = isGrid ? (contentX > (r.left + r.right) / 2)
                        : (contentY > (r.top  + r.bottom) / 2);
    return nearest + (after ? 1 : 0);
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
    int clientH = menuH_ - searchBoxH_ - classicFooterH_;
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
        int enabledCount = 4;  // Winzoo Settings + All Apps + Run dialog + Terminal always present
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
        btnPt.y = menuH_ - (5 + (settings_->appMenuSidebarShowExplorer ? 1 : 0)
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
    } else if (idx == 5) {
        // Open Windows Run dialog
        ShowRunDialog(hwnd_);
        closeReason_ = AppMenuCloseReason::Selection;
        done_ = true;
        DestroyWindow(hwnd_);
    } else if (idx == 6) {
        // Open Windows Terminal
        LaunchMenuApp(hwnd_, settings_->openAppsOnSameMonitor, L"wt.exe");
        closeReason_ = AppMenuCloseReason::Selection;
        done_ = true;
        DestroyWindow(hwnd_);
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
            bool isRoundedMode = settings_ && settings_->appMenuLayout == AppMenuLayout::ClassicRounded;
            bool isClassicMode = settings_ && (settings_->appMenuLayout == AppMenuLayout::Classic ||
                                               settings_->appMenuLayout == AppMenuLayout::ClassicRounded);
            int editX, editY, editW, editH;
            if (isRoundedMode) {
                int bInset    = Scale(8, dpi_);
                int bVInset   = Scale(3, dpi_);
                int iconPad   = Scale(6, dpi_);
                int iconAreaW = Scale(16, dpi_);
                editX = bInset + iconPad + iconAreaW + Scale(2, dpi_);
                editY = menuH_ - searchBoxH_ + bVInset + Scale(2, dpi_);
                editW = menuW_ - editX - bInset - Scale(4, dpi_);
                editH = searchBoxH_ - 2 * bVInset - Scale(4, dpi_);
            } else {
                int sbPad     = Scale(4, dpi_);
                int iconAreaW = Scale(20, dpi_);
                editX = sbPad + iconAreaW;
                editY = menuH_ - searchBoxH_ + Scale(3, dpi_);
                editW = (isClassicMode ? menuW_ + classicPanelW_ : menuW_) - editX - sbPad;
                editH = searchBoxH_ - Scale(6, dpi_);
            }

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
                const wchar_t* cue = isClassicMode ? L"Search programs and files" : L"Search...";
                SendMessageW(searchEdit_, EM_SETCUEBANNER, TRUE,
                             reinterpret_cast<LPARAM>(cue));

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

    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        // Begin a potential drag-reorder only on a pinned entry, and only in the normal
        // root view (not while searching or in Classic All-Programs).
        if (nodes_ == &ownedNodes_ && pinnedCount_ > 0) {
            int idx = HitTestEntry(pt);
            if (idx >= 0 && idx < pinnedCount_) {
                POINT scr = pt; ClientToScreen(hwnd, &scr);
                drag_.OnButtonDown(idx, scr);
                SetCapture(hwnd);
            }
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (drag_.State() != DragState::Idle) {
            POINT scr = pt; ClientToScreen(hwnd, &scr);
            drag_.OnMouseMove(scr);
            if (drag_.State() == DragState::Dragging) {
                dropIndex_ = ComputeDropIndex(pt);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
        }
        int newIdx       = HitTestEntry(pt);
        int newSidebarBtn = HitTestSidebarBtn(pt);
        int newClassicRight = HitTestClassicRight(pt);
        bool newFooter   = HitTestClassicFooter(pt);
        if (newIdx != hoveredIdx_ || newSidebarBtn != sidebarHoveredBtn_ ||
            newClassicRight != classicHoveredRight_ || newFooter != classicFooterHovered_) {
            hoveredIdx_           = newIdx;
            sidebarHoveredBtn_    = newSidebarBtn;
            classicHoveredRight_  = newClassicRight;
            classicFooterHovered_ = newFooter;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (drag_.State() != DragState::Idle) {
            bool wasDragging = (drag_.State() == DragState::Dragging);
            // Capture drag state BEFORE ReleaseCapture(): it synchronously sends
            // WM_CAPTURECHANGED, whose handler resets drag_ (clearing dragIndex/dropIndex).
            int dragIndex = drag_.DragIndex();
            int dropAt    = (dropIndex_ >= 0) ? dropIndex_ : ComputeDropIndex(pt);
            drag_.OnButtonUp();
            ReleaseCapture();
            dropIndex_ = -1;
            if (wasDragging) {
                // Reorder by path so uninstalled (skipped) pins keep their slots.
                if (dragIndex >= 0 && dragIndex < pinnedCount_ && nodes_ == &ownedNodes_) {
                    std::vector<std::wstring> displayed;
                    displayed.reserve(pinnedCount_);
                    for (int i = 0; i < pinnedCount_; ++i) {
                        const AppTreeNode& n = ownedNodes_[i];
                        displayed.push_back(n.exePath.empty() ? n.iconPath : n.exePath);
                    }
                    std::wstring moved = displayed[dragIndex];
                    displayed.erase(displayed.begin() + dragIndex);
                    int ins = dropAt;
                    if (ins > dragIndex) --ins;  // account for the removal
                    ins = std::max(0, std::min(ins, static_cast<int>(displayed.size())));
                    displayed.insert(displayed.begin() + ins, moved);

                    std::vector<std::wstring>& mut = MutableActivePins(*pinSettings_, monitorDeviceName_);
                    std::vector<std::wstring> undisplayed;
                    for (const auto& p : mut) {
                        bool shown = std::any_of(displayed.begin(), displayed.end(),
                            [&](const std::wstring& d){ return _wcsicmp(d.c_str(), p.c_str()) == 0; });
                        if (!shown) undisplayed.push_back(p);
                    }
                    mut = std::move(displayed);
                    mut.insert(mut.end(), undisplayed.begin(), undisplayed.end());
                    OnPinsMutated();
                }
                return 0;
            }
            // Not a drag (just a click) — fall through to normal activation below.
        }
        if (classicPanelW_ > 0) {
            // Classic layout: check right panel and footer first
            if (HitTestClassicFooter(pt)) {
                ToggleAllPrograms();
                return 0;
            }
            int rightIdx = HitTestClassicRight(pt);
            if (rightIdx >= 0) {
                if (settings_ && rightIdx == static_cast<int>(BuildClassicLinks(*settings_).size())
                    && settings_->appMenuClassicShowShutDown)
                {
                    // Defer the power popup via PostMessage so TrackPopupMenu
                    // runs in a fresh dispatch, not nested inside WM_LBUTTONUP.
                    PostMessageW(hwnd, WM_CLASSIC_POWER, 0, 0);
                } else {
                    ActivateClassicRight(rightIdx);
                }
                return 0;
            }
        }
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

    case WM_RBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int idx = HitTestEntry(pt);
        if (idx >= 0) {
            POINT scr = pt; ClientToScreen(hwnd, &scr);
            ShowEntryContextMenu(idx, scr);
        }
        return 0;
    }

    case WM_CAPTURECHANGED:
        drag_.OnCaptureChanged();
        dropIndex_ = -1;
        return 0;

    case WM_PINS_CHANGED:
        // A submenu changed the pin list — rebuild this (root) menu's pinned section.
        if (!isSubmenu_ && !ownedEntries_.empty())
            RebuildRootNodes();
        return 0;

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        int step = 0;
        if (settings_ && settings_->appMenuLayout == AppMenuLayout::Grid &&
            settings_->appMenuGridCols > 0 && !entryRects_.empty() &&
            GET_X_LPARAM(lParam) < menuW_)  // only scroll left panel for Classic
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
        bool isList = !settings_ || settings_->appMenuLayout == AppMenuLayout::List
                                 || settings_->appMenuLayout == AppMenuLayout::Classic
                                 || settings_->appMenuLayout == AppMenuLayout::ClassicRounded;
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

        case VK_PRIOR: // Page Up
        {
            if (count == 0) break;
            if (hoveredIdx_ == -1) {
                SetHoveredIdx(0);
                break;
            }
            int pageH = menuH_ - searchBoxH_;
            int rowH = (!entryRects_.empty())
                ? (std::cmp_greater_equal(entryRects_.size(), cols)
                    ? entryRects_[cols - 1].bottom
                    : entryRects_.back().bottom)
                : Scale(settings_ ? settings_->appMenuEntryHeight : 36, dpi_);
            int rows = std::max(1, pageH / std::max(1, rowH));
            SetHoveredIdx(std::max(0, hoveredIdx_ - rows * cols));
            break;
        }

        case VK_NEXT: // Page Down
        {
            if (count == 0) break;
            if (hoveredIdx_ == -1) {
                SetHoveredIdx(count - 1);
                break;
            }
            int pageH = menuH_ - searchBoxH_;
            int rowH = (!entryRects_.empty())
                ? (std::cmp_greater_equal(entryRects_.size(), cols)
                    ? entryRects_[cols - 1].bottom
                    : entryRects_.back().bottom)
                : Scale(settings_ ? settings_->appMenuEntryHeight : 36, dpi_);
            int rows = std::max(1, pageH / std::max(1, rowH));
            SetHoveredIdx(std::min(count - 1, hoveredIdx_ + rows * cols));
            break;
        }

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

    case WM_CLASSIC_POWER:
        if (settings_ && classicPanelW_ > 0) {
            auto links = BuildClassicLinks(*settings_);
            ActivateClassicRight(static_cast<int>(links.size()));
        }
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
        // settingsIcon_/sysToolIcon_ may have fallen back to the shared
        // IDI_APPLICATION (same handle every call) — don't DestroyIcon that.
        if (settingsIcon_) { if (settingsIcon_ != LoadIcon(nullptr, IDI_APPLICATION)) DestroyIcon(settingsIcon_); settingsIcon_ = nullptr; }
        if (sysToolIcon_)  { if (sysToolIcon_  != LoadIcon(nullptr, IDI_APPLICATION)) DestroyIcon(sysToolIcon_);  sysToolIcon_  = nullptr; }
        if (profilePicBmp_)  { DeleteObject(profilePicBmp_); profilePicBmp_  = nullptr; }
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
    HWND* pChildHwnd,
    int pinnedCount,
    std::vector<AppEntry> entries,
    const std::wstring& monitorDeviceName,
    std::function<void(const Settings&)> onSettingsChanged,
    Settings* pinSettings,
    HWND rootHwnd,
    bool taskbarFolder,
    std::function<void(const std::wstring&)> onRemoveFromFolder,
    std::function<void(const std::wstring&)> onSetFolderCover)
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

    // Context for user-pinned apps. Submenus share the root's settings object so pins
    // mutated from any level go to the same list.
    menu.pinnedCount_       = pinnedCount;
    menu.ownedEntries_      = std::move(entries);
    menu.monitorDeviceName_ = monitorDeviceName;
    menu.onSettingsChanged_ = std::move(onSettingsChanged);
    menu.pinSettings_       = pinSettings ? pinSettings : &menu.ownedSettings_;
    menu.isTaskbarFolder_   = taskbarFolder;
    menu.onRemoveFromFolder_= std::move(onRemoveFromFolder);
    menu.onSetFolderCover_  = std::move(onSetFolderCover);

    // Search: only on root menu when enabled in settings
    menu.searchEnabled_ = !isSubmenu && settings.appMenuSearchEnabled;
    menu.searchBoxH_    = menu.SearchBoxHeight();

    // Warm the (background) God-Mode enumeration now so the first system-tool
    // search returns dynamic results instead of the curated fallback.
    if (menu.searchEnabled_ && settings.appMenuSearchSystem &&
        settings.appMenuSearchSystemDynamic)
        PrewarmSystemTools();

    int menuW = Scale(settings.appMenuWidth, dpi);
    menu.menuW_ = menuW;

    bool isClassic = (!isSubmenu && (settings.appMenuLayout == AppMenuLayout::Classic ||
                                     settings.appMenuLayout == AppMenuLayout::ClassicRounded));
    bool isRounded = (!isSubmenu && settings.appMenuLayout == AppMenuLayout::ClassicRounded);

    // Sidebar: only on root menu (not submenus), and only when enabled and not Classic
    int sidebarW = (!isSubmenu && !isClassic && settings.appMenuSidebarEnabled)
                       ? Scale(settings.appMenuSidebarWidth, dpi) : 0;
    menu.sidebarW_ = sidebarW;

    // Classic right panel
    int classicPanelW = isClassic ? Scale(settings.appMenuClassicPanelWidth, dpi) : 0;
    menu.classicPanelW_ = classicPanelW;

    int totalW = menuW + sidebarW + classicPanelW;

    // Classic footer row (All Programs / Back) — ClassicRounded uses a shorter row
    int classicFooterH = isClassic ? (isRounded ? Scale(26, dpi) : Scale(settings.appMenuEntryHeight, dpi)) : 0;
    menu.classicFooterH_ = classicFooterH;

    menu.BuildEntryRects(menuW);
    int padPx    = Scale(settings.appMenuPadding, dpi);
    int contentH = ContentHeight(menu.entryRects_) + padPx;

    int maxH = 0;
    if (settings.appMenuLayout == AppMenuLayout::Grid &&
        !menu.entryRects_.empty() && settings.appMenuGridCols > 0)
    {
        int cols  = std::max(1, settings.appMenuGridCols);
        // All grid cells share one height; derive it from the first cell so a pinned-row
        // break (which can push entryRects_[cols-1] onto a later row) doesn't inflate it.
        int cellH = menu.entryRects_[0].bottom - menu.entryRects_[0].top;
        (void)cols;
        int maxRows = std::max(1, settings.appMenuGridRows);
        maxH = std::min(Scale(settings.appMenuMaxHeight, dpi), 2 * padPx + cellH * maxRows);
    } else {
        maxH = Scale(settings.appMenuMaxHeight, dpi);
    }

    // Add search box and classic footer height to total menu height
    int menuH = std::min(contentH + menu.searchBoxH_ + classicFooterH,
                         maxH    + menu.searchBoxH_ + classicFooterH);
    menu.menuH_ = menuH;
    menu.UpdateMaxScroll(contentH, menuH - menu.searchBoxH_ - classicFooterH);

    // Classic: load profile picture and pre-build all-programs list
    if (isClassic) {
        int picSize = Scale(48, dpi);
        menu.profilePicBmp_ = LoadUserProfilePicture(picSize);
    }

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

    DWORD wndStyle = isRounded ? WS_POPUP : (WS_POPUP | WS_BORDER);
    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST,
        kAppMenuClass, nullptr,
        wndStyle,
        x, y, totalW, menuH,
        hwndOwner, nullptr, hInst, &menu);

    if (!hwnd) return AppMenuCloseReason::ClickedOutside;

    // Root menu's own window is the refresh target; submenus inherit the root's hwnd.
    menu.rootHwnd_ = rootHwnd ? rootHwnd : hwnd;

    if (isRounded) {
        // Clip the window to a rounded rectangle. CreateRoundRectRgn's last two
        // parameters are the ellipse width/height; radius = diameter / 2.
        int ellipse = Scale(12, dpi) * 2; // 12 px corner radius at 96 DPI
        HRGN rgn = CreateRoundRectRgn(0, 0, totalW + 1, menuH + 1, ellipse, ellipse);
        SetWindowRgn(hwnd, rgn, FALSE);
        // Also request DWM smooth rounding on Windows 11 (attribute 33 = DWMWA_WINDOW_CORNER_PREFERENCE).
        // Falls back silently on Windows 10 where the attribute is unsupported.
        constexpr DWORD DWMWCP_ROUND_VAL = 2;
        DwmSetWindowAttribute(hwnd, 33, &DWMWCP_ROUND_VAL, sizeof(DWORD));
    }

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
                          const ThemeColors& colors, int dpi,
                          const std::wstring& monitorDeviceName,
                          std::function<void(const Settings&)> onSettingsChanged)
{
    if (entries.empty()) return;
    int pinnedCount = 0;
    std::vector<AppTreeNode> tree = BuildRootNodes(entries, settings, monitorDeviceName, pinnedCount);
    if (tree.empty()) return;

    ShowNodes(hwndOwner, startBtnScreenRect, /*isSubmenu=*/false, position,
              std::move(tree), settings, colors, dpi,
              /*pChildHwnd=*/nullptr, pinnedCount, std::move(entries),
              monitorDeviceName, std::move(onSettingsChanged));
}

void AppMenuWindow::ShowFolder(HWND hwndOwner, RECT anchorScreenRect,
                               TaskbarPosition position,
                               const std::wstring& folderName,
                               std::vector<AppTreeNode> appNodes,
                               const Settings& settings,
                               const ThemeColors& colors, int dpi,
                               const std::wstring& monitorDeviceName,
                               std::function<void(const Settings&)> onSettingsChanged,
                               std::function<void(const std::wstring&)> onRemoveFromFolder,
                               std::function<void(const std::wstring&)> onSetFolderCover)
{
    (void)folderName;  // no title chrome on the bare grid popup
    if (appNodes.empty()) return;

    // Force a clean grid popup. isSubmenu=false selects ShowNodes' taskbar-edge
    // anchoring; disabling the root-only chrome (search/sidebar/classic) means none
    // of it renders, leaving a bare grid that pops off the taskbar edge.
    Settings s = settings;
    s.appMenuLayout         = AppMenuLayout::Grid;
    s.appMenuSearchEnabled  = false;
    s.appMenuSidebarEnabled = false;

    ShowNodes(hwndOwner, anchorScreenRect, /*isSubmenu=*/false, position,
              std::move(appNodes), s, colors, dpi,
              /*pChildHwnd=*/nullptr, /*pinnedCount=*/0, /*entries=*/{},
              monitorDeviceName, std::move(onSettingsChanged),
              /*pinSettings=*/nullptr, /*rootHwnd=*/nullptr,
              /*taskbarFolder=*/true, std::move(onRemoveFromFolder),
              std::move(onSetFolderCover));
}
