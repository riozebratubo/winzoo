#include "TaskbarWindow.h"
#include "App.h"
#include "Registry.h"
#include "SettingsFile.h"
#include "resource.h"
#include "SystemStatus.h"
#include "LaunchHelper.h"
#include "JumpList.h"
#include <algorithm>
#include <atomic>
#include <memory>
#include <unordered_set>
#include <utility>
#include <commctrl.h>
#include <windowsx.h>
#include <objbase.h>
#include <initguid.h>
#include <msctf.h>

static std::wstring ApplyClockPattern(const std::wstring& pattern, const SYSTEMTIME& st, bool isTime)
{
    std::wstring result = pattern;

    auto sub = [&](const wchar_t* token, int value, int digits) {
        wchar_t buf[8];
        swprintf_s(buf, digits == 4 ? L"%04d" : L"%02d", value);
        size_t pos = 0;
        while ((pos = result.find(token, pos)) != std::wstring::npos) {
            result.replace(pos, wcslen(token), buf);
            pos += static_cast<size_t>(digits);
        }
    };

    if (isTime) {
        sub(L"$hh", st.wHour,   2);
        sub(L"$mm", st.wMinute, 2);
        sub(L"$ss", st.wSecond, 2);
    } else {
        sub(L"$yyyy", st.wYear,        4);  // before $yy to avoid partial match
        sub(L"$yy",   st.wYear % 100,  2);
        sub(L"$dd",   st.wDay,         2);
        sub(L"$mm",   st.wMonth,       2);
    }

    return result;
}

// Returns "EN-US" / "ZH-TW" / "PT-BR" style text for a LANGID.
// Uses ISO 639-1 (2-letter lang) + ISO 3166-1 (2-letter country), always uppercase.
static std::wstring GetLangDisplayText(LANGID langId)
{
    LCID lcid = MAKELCID(langId, SORT_DEFAULT);
    wchar_t lang[16] = {}, ctry[16] = {};
    bool gotLang = GetLocaleInfoW(lcid, LOCALE_SISO639LANGNAME,  lang, _countof(lang)) > 0 && lang[0];
    bool gotCtry = GetLocaleInfoW(lcid, LOCALE_SISO3166CTRYNAME, ctry, _countof(ctry)) > 0 && ctry[0];
    if (gotLang) {
        CharUpperW(lang);
        if (gotCtry) { CharUpperW(ctry); return std::wstring(lang) + L"-" + ctry; }
        return lang;
    }
    // Fallback: abbreviated name (e.g. "ENU"), trimmed to 3 chars
    wchar_t abbr[16] = {};
    if (GetLocaleInfoW(lcid, LOCALE_SABBREVLANGNAME, abbr, _countof(abbr)) > 0 && abbr[0]) {
        abbr[3] = L'\0';
        CharUpperW(abbr);
        return abbr;
    }
    return {};
}

// Returns {display text, HKL} for the foreground window's active input language.
// Primary: GetKeyboardLayout on the foreground thread (fast, no COM).
// TSF fallback: ITfInputProcessorProfileMgr (handles unusual/custom input processors).
static std::pair<std::wstring, HKL> GetCurrentInputLanguage()
{
    HWND fg = GetForegroundWindow();
    if (fg) {
        DWORD tid = GetWindowThreadProcessId(fg, nullptr);
        if (tid) {
            HKL hkl = GetKeyboardLayout(tid);
            if (hkl) {
                LANGID langId = static_cast<LANGID>(
                    reinterpret_cast<ULONG_PTR>(hkl) & 0xFFFF);
                std::wstring text = GetLangDisplayText(langId);
                if (!text.empty()) return { text, hkl };
            }
        }
    }
    // TSF fallback
    ITfInputProcessorProfileMgr* pMgr = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                   CLSCTX_INPROC_SERVER,
                                   IID_ITfInputProcessorProfileMgr,
                                   reinterpret_cast<void**>(&pMgr)))) {
        TF_INPUTPROCESSORPROFILE profile = {};
        std::wstring text;
        if (SUCCEEDED(pMgr->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &profile)))
            text = GetLangDisplayText(profile.langid);
        pMgr->Release();
        if (!text.empty())
            return { text, GetKeyboardLayout(GetCurrentThreadId()) };
    }
    return {};
}

static constexpr wchar_t kClassName[] = L"WinzooTaskbar";

bool TaskbarWindow::RegisterWndClass(HINSTANCE hInst)
{
    WNDCLASSEXW existing = {};
    existing.cbSize = sizeof(existing);
    if (GetClassInfoExW(hInst, kClassName, &existing)) return true;

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = TaskbarWindow::WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    wc.hIcon         = LoadIcon(hInst, MAKEINTRESOURCE(IDI_APPICON));
    return RegisterClassExW(&wc) != 0;
}

LRESULT CALLBACK TaskbarWindow::WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    TaskbarWindow* pThis = nullptr;

    if (uMsg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        pThis = static_cast<TaskbarWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        pThis->hwnd_ = hwnd;
    } else {
        pThis = reinterpret_cast<TaskbarWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (pThis)
        return pThis->HandleMessage(hwnd, uMsg, wParam, lParam);
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

bool TaskbarWindow::Create(HINSTANCE hInst, const Settings& settings,
                           HMONITOR hMonitor, bool isPrimary)
{
    hInst_    = hInst;
    settings_ = settings;
    colors_   = GetThemeColors(settings.theme);
    dpi_      = GetWindowDpi(nullptr);
    hMonitor_ = hMonitor;
    isPrimary_= isPrimary;
    UpdateMonitorDeviceName();
    showStartButton_ = isPrimary_
                     || settings_.showAppMenuOnAllMonitors
                     || settings_.taskbarMonitorMode == TaskbarMonitorMode::Primary;

    if (!RegisterWndClass(hInst)) return false;

    // Register messages before window creation
    appBarCallbackMsg_ = RegisterWindowMessage(L"WinzooAppBarCallback");
    taskbarCreatedMsg_ = RegisterWindowMessage(L"TaskbarCreated");

    RECT rc = CalculateWindowRect();

    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        kClassName, L"Winzoo",
        WS_POPUP,
        rc.left, rc.top,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInst, this);

    return hwnd != nullptr;
}

void TaskbarWindow::SetMonitor(HMONITOR hMonitor, bool isPrimary)
{
    hMonitor_  = hMonitor;
    isPrimary_ = isPrimary;
    UpdateMonitorDeviceName();
    showStartButton_ = isPrimary_
                     || settings_.showAppMenuOnAllMonitors
                     || settings_.taskbarMonitorMode == TaskbarMonitorMode::Primary;
}

int TaskbarWindow::EffectiveThicknessPx() const
{
    bool isVertical = settings_.position == TaskbarPosition::Left
                   || settings_.position == TaskbarPosition::Right;
    int raw = (isVertical && settings_.showTitlesOnVertical)
            ? settings_.leftRightHeight
            : settings_.thickness;
    return Scale(raw, dpi_);
}

RECT TaskbarWindow::CalculateWindowRect() const
{
    // Use the assigned monitor; fall back to primary if not set
    HMONITOR hMon = hMonitor_
                  ? hMonitor_
                  : MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfo(hMon, &mi);
    RECT mon = mi.rcMonitor;

    int thick   = Scale(settings_.thickness, dpi_);
    int thickLR = EffectiveThicknessPx();

    switch (settings_.position) {
    case TaskbarPosition::Top:
        return { mon.left, mon.top, mon.right, mon.top + thick };
    case TaskbarPosition::Left:
        return { mon.left, mon.top, mon.left + thickLR, mon.bottom };
    case TaskbarPosition::Right:
        return { mon.right - thickLR, mon.top, mon.right, mon.bottom };
    case TaskbarPosition::Floating:
        return { settings_.floatX, settings_.floatY,
                 settings_.floatX + Scale(settings_.floatWidth, dpi_),
                 settings_.floatY + thick };
    default: // Bottom
        return { mon.left, mon.bottom - thick, mon.right, mon.bottom };
    }
}

void TaskbarWindow::Show()
{
    if (!hwnd_) return;
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd_);
}

void TaskbarWindow::Destroy()
{
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void TaskbarWindow::ApplySettings(const Settings& s)
{
    bool pinnedPathsChanged = (s.pinnedExePaths != settings_.pinnedExePaths)
                           || (s.pinnedExePathsPerMonitor != settings_.pinnedExePathsPerMonitor)
                           || (s.pinnedAppsPerMonitor != settings_.pinnedAppsPerMonitor);

    settings_ = s;
    colors_   = GetThemeColors(s.theme);
    showStartButton_ = isPrimary_
                     || settings_.showAppMenuOnAllMonitors
                     || settings_.taskbarMonitorMode == TaskbarMonitorMode::Primary;
    SaveSettings(s);

    if (settings_.position != TaskbarPosition::Floating) {
        if (!appBar_.IsRegistered())
            appBar_.Register(hwnd_, settings_.position, EffectiveThicknessPx());
        else
            appBar_.SetPosition(settings_.position, EffectiveThicknessPx());
        RECT rc = appBar_.GetReservedRect();
        SetWindowPos(hwnd_, HWND_TOPMOST,
                     rc.left, rc.top,
                     rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
        RECT client;
        GetClientRect(hwnd_, &client);
        HDC hdc = GetDC(hwnd_);
        renderer_.Resize(client.right, client.bottom, hdc);
        ReleaseDC(hwnd_, hdc);
    } else {
        appBar_.Unregister();
        RECT rc = CalculateWindowRect();
        SetWindowPos(hwnd_, HWND_TOPMOST,
                     rc.left, rc.top,
                     rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOACTIVATE);
    }

    RebuildPinnedButtons();

    if (pinnedPathsChanged) {
        for (const auto& btn : pinnedButtons_) {
            if (!btn.icon) { StartIconLoadThread(); break; }
        }
    }

    LayoutButtons();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ─── Combined-index helpers ──────────────────────────────────────────────────

int TaskbarWindow::TotalCount() const
{
    return (int)pinnedButtons_.size() + (int)tracker_.Buttons().size();
}

bool TaskbarWindow::IsPinnedIdx(int i) const
{
    return i >= 0 && std::cmp_less(i, pinnedButtons_.size());
}

const TaskButton& TaskbarWindow::GetButtonByIdx(int i) const
{
    if (IsPinnedIdx(i)) return pinnedButtons_[i];
    return tracker_.Buttons()[i - (int)pinnedButtons_.size()];
}

TaskButton& TaskbarWindow::GetMutableButtonByIdx(int i)
{
    if (IsPinnedIdx(i)) return pinnedButtons_[i];
    return tracker_.MutableButtons()[i - (int)pinnedButtons_.size()];
}

HWND TaskbarWindow::FindHwndByExePath(const std::wstring& exePath) const
{
    for (const auto& btn : tracker_.Buttons()) {
        if (!btn.exePath.empty() &&
            _wcsicmp(btn.exePath.c_str(), exePath.c_str()) == 0)
            return btn.hwnd;
    }
    return nullptr;
}

bool TaskbarWindow::IsExeRunning(const std::wstring& exePath) const
{
    return FindHwndByExePath(exePath) != nullptr;
}

/*static*/ std::wstring TaskbarWindow::ExeBaseName(const std::wstring& exePath)
{
    size_t pos = exePath.rfind(L'\\');
    std::wstring name = (pos != std::wstring::npos) ? exePath.substr(pos + 1) : exePath;
    size_t ext = name.rfind(L'.');
    if (ext != std::wstring::npos) name = name.substr(0, ext);
    return name;
}

void TaskbarWindow::UpdateMonitorDeviceName()
{
    monitorDeviceName_.clear();
    if (!hMonitor_) return;
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMonitor_, &mi)) {
        const wchar_t* name = mi.szDevice;
        if (wcsncmp(name, L"\\\\.\\", 4) == 0)
            name += 4;
        monitorDeviceName_ = name;
    }
}

void TaskbarWindow::RebuildPinnedButtons()
{
    int iconSz = Scale(48, dpi_);

    const std::vector<std::wstring>* paths = nullptr;
    static const std::vector<std::wstring> kEmpty;
    std::vector<std::wstring> mergedPaths;
    if (settings_.pinnedAppsPerMonitor && settings_.position == TaskbarPosition::Floating) {
        // Merge all per-monitor pin lists, deduplicated by exe path.
        std::unordered_set<std::wstring> seen;
        for (auto& [mon, monPaths] : settings_.pinnedExePathsPerMonitor) {
            for (auto& p : monPaths) {
                std::wstring lower = p;
                CharLowerW(lower.data());
                if (seen.insert(lower).second)
                    mergedPaths.push_back(p);
            }
        }
        paths = &mergedPaths;
    } else if (settings_.pinnedAppsPerMonitor && !monitorDeviceName_.empty()) {
        auto it = settings_.pinnedExePathsPerMonitor.find(monitorDeviceName_);
        paths = (it != settings_.pinnedExePathsPerMonitor.end()) ? &it->second : &kEmpty;
    } else {
        paths = &settings_.pinnedExePaths;
    }

    std::vector<TaskButton> newPinned;
    newPinned.reserve(paths->size());
    for (const auto& path : *paths) {
        TaskButton btn;
        btn.exePath  = path;
        btn.isPinned = true;
        btn.iconOnly = true;
        btn.title    = ExeBaseName(path);
        btn.icon     = appIconCache_.TryGet(path, iconSz);
        newPinned.push_back(std::move(btn));
    }
    pinnedButtons_ = std::move(newPinned);
}

// Forward a mouse notification to a tray icon's owner window.
// Sends both NOTIFYICON_VERSION_4 format (modern) and VERSION_3 format (legacy)
// so all apps respond regardless of which version they registered with.
static void ForwardTrayNotification(HWND hWnd, UINT callbackMsg, UINT uID,
                                    POINT screenPt, bool rightClick)
{
    if (!hWnd || !callbackMsg) return;

    // For right-click menus: target window must be foreground so TrackPopupMenu works.
    SetForegroundWindow(hWnd);

    const UINT down = rightClick ? WM_RBUTTONDOWN : WM_LBUTTONDOWN;
    const UINT up   = rightClick ? WM_RBUTTONUP   : WM_LBUTTONUP;
    const UINT ctx  = rightClick ? WM_CONTEXTMENU : 0;

    // VERSION_4 format: wParam = screen coords, lParam = MAKELPARAM(notification, uID)
    WPARAM wp4 = MAKEWPARAM(static_cast<WORD>(screenPt.x), static_cast<WORD>(screenPt.y));
    PostMessage(hWnd, callbackMsg, wp4, MAKELPARAM(down, uID));
    PostMessage(hWnd, callbackMsg, wp4, MAKELPARAM(up,   uID));
    if (rightClick)
        PostMessage(hWnd, callbackMsg, wp4, MAKELPARAM(ctx, uID));
    else
        PostMessage(hWnd, callbackMsg, wp4, MAKELPARAM(NIN_SELECT, uID));

    // VERSION_3 format: wParam = uID, lParam = notification (fallback for older apps).
    // v4 apps will ignore these because their uID check is in HIWORD(lParam), not wParam.
    PostMessage(hWnd, callbackMsg, static_cast<WPARAM>(uID), static_cast<LPARAM>(down));
    PostMessage(hWnd, callbackMsg, static_cast<WPARAM>(uID), static_cast<LPARAM>(up));
}

void TaskbarWindow::RefreshTrayIcons()
{
    if (!settings_.showTrayIcons) {
        for (auto& e : trayIcons_) if (e.hIcon) { DestroyIcon(e.hIcon); e.hIcon = nullptr; }
        trayIcons_.clear();
        return;
    }

    int iconPx = std::min(Scale(settings_.trayIconSize, dpi_),
                          Scale(settings_.thickness, dpi_));
    auto fresh = EnumerateTrayIcons(iconPx, settings_.trayIconFallbackExe,
                                    settings_.showOverflowTrayIcons);

    // If enumeration returned nothing but we already have entries, keep the
    // previous set. This handles the case where Explorer's toolbar becomes
    // temporarily inaccessible (e.g. after Shell_TrayWnd is hidden).
    if (fresh.empty() && !trayIcons_.empty())
        return;

    // --- Sort by settings_.trayIconOrder ---
    // Build an index map: orderKey → position in the saved order.
    std::vector<std::wstring>& order = settings_.trayIconOrder;

    // Separate fresh icons into known (in order) and unknown (new).
    std::vector<TrayIconEntry> known, unknown;
    for (auto& e : fresh) {
        auto it = std::find(order.begin(), order.end(), e.orderKey);
        if (it != order.end())
            known.push_back(std::move(e));
        else
            unknown.push_back(std::move(e));
    }

    // Sort known icons by their position in the order list.
    std::sort(known.begin(), known.end(), [&](const TrayIconEntry& a, const TrayIconEntry& b) {
        auto ia = std::find(order.begin(), order.end(), a.orderKey);
        auto ib = std::find(order.begin(), order.end(), b.orderKey);
        return std::distance(order.begin(), ia) < std::distance(order.begin(), ib);
    });

    // Prepend new (unknown) icons at the front of the order list and result.
    if (!unknown.empty()) {
        std::vector<std::wstring> newKeys;
        newKeys.reserve(unknown.size());
        for (const auto& e : unknown) newKeys.push_back(e.orderKey);
        order.insert(order.begin(), newKeys.begin(), newKeys.end());
        SaveSettings(settings_);
    }

    // Merge: new icons first, then known icons in order.
    std::vector<TrayIconEntry> sorted;
    sorted.reserve(unknown.size() + known.size());
    for (auto& e : unknown) sorted.push_back(std::move(e));
    for (auto& e : known)   sorted.push_back(std::move(e));

    // Carry over cached icons: if a fresh entry has no icon but the previous
    // trayIcons_ had one for the same orderKey, transfer it to the new entry.
    for (auto& newEntry : sorted) {
        if (newEntry.hIcon) continue;
        for (auto& oldEntry : trayIcons_) {
            if (oldEntry.hIcon && oldEntry.orderKey == newEntry.orderKey) {
                newEntry.hIcon = oldEntry.hIcon;
                oldEntry.hIcon = nullptr; // transferred, don't destroy
                break;
            }
        }
    }

    // Destroy old icons and replace.
    for (auto& e : trayIcons_) if (e.hIcon) { DestroyIcon(e.hIcon); e.hIcon = nullptr; }
    trayIcons_ = std::move(sorted);

    LayoutButtons();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void TaskbarWindow::ComputeClockFontSizes()
{
    if (!settings_.showClock) {
        fittedTimePt_ = settings_.clockTimeFontSize;
        fittedDatePt_ = settings_.clockDateFontSize;
        return;
    }

    bool isVertical = settings_.position == TaskbarPosition::Left
                   || settings_.position == TaskbarPosition::Right;
    int baseW = isVertical ? EffectiveThicknessPx() : Scale(settings_.clockWidth, dpi_);

    if (dpi_                          == clockFitDpi_    &&
        baseW                         == clockFitW_      &&
        settings_.clockTimeFontSize   == clockFitTimePt_ &&
        settings_.clockDateFontSize   == clockFitDatePt_ &&
        settings_.clockTimeFormat     == clockFitTimeFmt_ &&
        settings_.clockDateFormat     == clockFitDateFmt_)
        return;

    int availW = baseW - Scale(4, dpi_) - 50;

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    std::wstring timeSample = ApplyClockPattern(settings_.clockTimeFormat, st, true);
    std::wstring dateSample = ApplyClockPattern(settings_.clockDateFormat, st, false);

    int timePt = settings_.clockTimeFontSize;
    while (timePt > 6 && availW > 0) {
        if (renderer_.MeasureSmallText(timeSample, timePt, dpi_) <= availW) break;
        --timePt;
    }

    int datePt = settings_.clockDateFontSize;
    while (datePt > 6 && availW > 0) {
        if (renderer_.MeasureSmallText(dateSample, datePt, dpi_) <= availW) break;
        --datePt;
    }

    fittedTimePt_ = timePt;
    fittedDatePt_ = datePt;

    clockFitDpi_    = dpi_;
    clockFitW_      = baseW;
    clockFitTimePt_ = settings_.clockTimeFontSize;
    clockFitDatePt_ = settings_.clockDateFontSize;
    clockFitTimeFmt_ = settings_.clockTimeFormat;
    clockFitDateFmt_ = settings_.clockDateFormat;
}

void TaskbarWindow::LayoutButtons()
{
    if (!hwnd_) return;

    ComputeClockFontSizes();

    RECT client;
    GetClientRect(hwnd_, &client);
    int w = client.right  - client.left;
    int h = client.bottom - client.top;

    auto& taskBtns = tracker_.MutableButtons();

    // Determine visible task buttons (monitor filter)
    bool filterByMonitor = settings_.showCurrentMonitorAppsOnly
                        && settings_.taskbarMonitorMode == TaskbarMonitorMode::AllMonitors
                        && settings_.position != TaskbarPosition::Floating
                        && hMonitor_ != nullptr;

    std::vector<TaskButton*> visibleTask;
    visibleTask.reserve(taskBtns.size());
    for (auto& btn : taskBtns) {
        bool show = true;
        if (filterByMonitor) {
            if (btn.hwnd)
                show = (MonitorFromWindow(btn.hwnd, MONITOR_DEFAULTTONULL) == hMonitor_);
            else
                show = isPrimary_;
        }
        if (show)
            visibleTask.push_back(&btn);
        else
            btn.rect = {};
    }

    // Determine which pinned buttons are visible
    std::vector<TaskButton*> visiblePinned;
    visiblePinned.reserve(pinnedButtons_.size());
    for (auto& btn : pinnedButtons_) {
        bool show = true;
        if (!settings_.pinnedAppsAsButtonsWhenOpen && IsExeRunning(btn.exePath))
            show = false;
        if (filterByMonitor && !isPrimary_ && !settings_.pinnedAppsPerMonitor)
            show = false;
        if (show)
            visiblePinned.push_back(&btn);
        else
            btn.rect = {};
    }

    int pCount = static_cast<int>(visiblePinned.size());
    int tCount = static_cast<int>(visibleTask.size());

    clockRect_       = {};
    scrollNeeded_    = false;
    scrollLeftRect_  = scrollRightRect_ = {};
    maxScrollOffset_ = 0;
    pinnedSepX_      = 0;
    statusZoneRect_  = {};
    volIconRect_     = {};
    netIconRect_     = {};
    batIconRect_     = {};
    langIconRect_    = {};
    trayZoneRect_    = {};
    trayIconRects_.clear();

    int startSz  = showStartButton_ ? Scale(settings_.thickness, dpi_) : 0;
    bool isHoriz = (settings_.position != TaskbarPosition::Left &&
                    settings_.position != TaskbarPosition::Right);
    int pad      = Scale(2, dpi_);
    // Status icons use the configured size (DPI-scaled), clamped to fit the taskbar
    int statusIconW = std::min(Scale(settings_.statusIconSize, dpi_), h - 2 * pad);

    // Status zone width (only on horizontal bars)
    int statusZoneW = 0;
    int langW = 0;
    if (settings_.showStatusZone && isHoriz) {
        if (statusData_.volAvailable && !settings_.hideDefaultTrayIcons) statusZoneW += statusIconW;
        if (statusData_.netAvailable && !settings_.hideDefaultTrayIcons) statusZoneW += statusIconW;
        if (statusData_.batAvailable) statusZoneW += statusIconW;
    }
    if (settings_.showLangIndicator && isHoriz && !currentLangText_.empty()) {
        int tw = renderer_.MeasureSmallText(currentLangText_,
                                            settings_.clockDateFontSize, dpi_);
        langW = (tw > 0 ? tw : Scale(40, dpi_)) + Scale(9, dpi_);
        statusZoneW += langW;
    }

    // Tray zone width (only on horizontal bars, to the left of the status zone)
    int traySlotW = 0;  // width of one icon slot (icon + margins)
    int trayZoneW = 0;
    if (settings_.showTrayIcons && isHoriz && !trayIcons_.empty()) {
        int iconPx = std::min(Scale(settings_.trayIconSize, dpi_), h - 2 * pad);
        int margPx = Scale(settings_.trayIconMargin, dpi_);
        int padPx  = Scale(settings_.trayIconPadding, dpi_);
        traySlotW  = iconPx + 2 * padPx;
        int n      = static_cast<int>(trayIcons_.size());
        trayZoneW  = n * traySlotW + (n > 1 ? (n - 1) * margPx : 0);
    }

    if (showStartButton_) {
        if (isHoriz)
            startBtnRect_ = { 0, pad, startSz, h - pad };
        else
            startBtnRect_ = { pad, 0, w - pad, startSz };
    } else {
        startBtnRect_ = {};
    }

    int maxBtnW = Scale(settings_.maxButtonWidth, dpi_);
    int minBtnW = Scale(settings_.minButtonWidth, dpi_);
    if (minBtnW > maxBtnW) minBtnW = maxBtnW;

    // Gap between the tray zone and the adjacent zone to its right (clock/status)
    int trayGap = (trayZoneW > 0) ? Scale(6, dpi_) : 0;

    // Tail reservation
    int tail = 0;
    if (settings_.showClock) {
        int cw = Scale(settings_.clockWidth, dpi_);
        tail = statusZoneW + trayZoneW + trayGap + cw;
        clockRect_ = isHoriz ? RECT{ w - cw, pad, w - pad, h - pad }
                              : RECT{ pad, h - cw, w - pad, h - pad };
    } else {
        tail = statusZoneW + trayZoneW + trayGap + (settings_.showRightClickGap ? Scale(20, dpi_) : 0);
    }

    // Status icon rects (to the left of the clock, or at the right edge)
    if (statusZoneW > 0 && isHoriz) {
        int szRight = settings_.showClock ? (w - Scale(settings_.clockWidth, dpi_))
                                           : w - (settings_.showRightClickGap ? Scale(20, dpi_) : 0);
        statusZoneRect_ = { szRight - statusZoneW, pad, szRight, h - pad };
        int iconTop = (h - statusIconW) / 2;
        int iconBot = iconTop + statusIconW;
        int x = statusZoneRect_.left;
        if (settings_.showStatusZone) {
            if (statusData_.volAvailable && !settings_.hideDefaultTrayIcons) {
                volIconRect_ = { x, iconTop, x + statusIconW, iconBot };
                x += statusIconW;
            }
            if (statusData_.netAvailable && !settings_.hideDefaultTrayIcons) {
                netIconRect_ = { x, iconTop, x + statusIconW, iconBot };
                x += statusIconW;
            }
            if (statusData_.batAvailable) {
                batIconRect_ = { x, iconTop, x + statusIconW, iconBot };
                x += statusIconW;
            }
        }
        if (langW > 0) {
            langIconRect_ = { x, iconTop, x + langW - Scale(3, dpi_), iconBot };
        }
    }

    // Tray icon rects (to the left of the status zone, with gap to the right)
    if (trayZoneW > 0 && isHoriz) {
        int tzRight  = statusZoneRect_.left > 0 ? statusZoneRect_.left - trayGap
                       : (settings_.showClock ? (w - Scale(settings_.clockWidth, dpi_) - trayGap)
                          : w - (settings_.showRightClickGap ? Scale(20, dpi_) : 0) - trayGap);
        trayZoneRect_ = { tzRight - trayZoneW, pad, tzRight, h - pad };

        int iconPx = std::min(Scale(settings_.trayIconSize, dpi_), h - 2 * pad);
        int margPx = Scale(settings_.trayIconMargin, dpi_);
        int padPx  = Scale(settings_.trayIconPadding, dpi_);
        int iconTop = (h - iconPx) / 2;
        int iconBot = iconTop + iconPx;
        int n = static_cast<int>(trayIcons_.size());
        trayIconRects_.resize(n);
        int x = trayZoneRect_.left;
        for (int i = 0; i < n; ++i) {
            trayIconRects_[i] = { x + padPx, iconTop, x + padPx + iconPx, iconBot };
            x += traySlotW + (i + 1 < n ? margPx : 0);
        }
    }

    if (isHoriz) {
        int arrowW = Scale(20, dpi_);

        // --- Pinned zone ---
        int pinnedZoneEnd = startSz; // default: no pinned buttons
        if (pCount > 0) {
            int x = startSz + pad;
            for (auto* btn : visiblePinned) {
                btn->rect = { x, pad, x + minBtnW, h - pad };
                x += minBtnW + pad;
            }
            pinnedZoneEnd = x;
            pinnedSepX_   = pinnedZoneEnd + Scale(2, dpi_);
        }

        // --- Task zone ---
        int taskStart = (pCount > 0) ? pinnedZoneEnd + Scale(5, dpi_) : startSz;
        int taskAvail = w - taskStart - tail;

        if (tCount > 0 && taskAvail > 0) {
            int totalMin = tCount * minBtnW + (tCount - 1) * pad + 2 * pad;
            if (totalMin <= taskAvail) {
                scrollOffset_ = 0;
                int area = taskAvail - 2 * pad;
                int btnW = std::min(maxBtnW,
                                    std::max(minBtnW,
                                             (area - (tCount - 1) * pad) / tCount));
                int x = taskStart + pad;
                for (auto* btn : visibleTask) {
                    btn->rect = { x, pad, x + btnW, h - pad };
                    x += btnW + pad;
                }
            } else {
                scrollNeeded_    = true;
                scrollLeftRect_  = { taskStart,                       0, taskStart + arrowW,    h };
                scrollRightRect_ = { taskStart + taskAvail - arrowW,  0, taskStart + taskAvail, h };
                int inner    = taskAvail - 2 * arrowW - 2 * pad;
                int visCount = std::max(1, (inner + pad) / (minBtnW + pad));
                maxScrollOffset_ = std::max(0, tCount - visCount);
                scrollOffset_    = std::min(scrollOffset_, maxScrollOffset_);
                for (auto* btn : visibleTask) btn->rect = {};
                int x = taskStart + arrowW + pad;
                for (int i = scrollOffset_; i < scrollOffset_ + visCount && i < tCount; ++i) {
                    visibleTask[i]->rect = { x, pad, x + minBtnW, h - pad };
                    x += minBtnW + pad;
                }
            }
        } else {
            for (auto& btn : taskBtns) btn.rect = {};
        }

    } else {
        // Vertical layout
        int arrowW = Scale(20, dpi_);
        bool showTitles = settings_.showTitlesOnVertical;
        int btnH = Scale(settings_.thickness, dpi_);

        for (auto* btn : visiblePinned) btn->iconOnly = !showTitles;
        for (auto* btn : visibleTask)   btn->iconOnly = !showTitles;

        // --- Pinned zone ---
        int pinnedZoneEnd = startSz;
        if (pCount > 0) {
            int y = startSz + pad;
            for (auto* btn : visiblePinned) {
                btn->rect = { pad, y, w - pad, y + btnH };
                y += btnH + pad;
            }
            pinnedZoneEnd = y;
            pinnedSepX_   = pinnedZoneEnd + Scale(2, dpi_); // Y position for horiz separator
        }

        // --- Task zone ---
        int taskStart = (pCount > 0) ? pinnedZoneEnd + Scale(5, dpi_) : startSz;
        int taskAvail = h - taskStart - tail;

        if (tCount > 0 && taskAvail > 0) {
            int totalMin = tCount * btnH + (tCount - 1) * pad + 2 * pad;
            if (totalMin <= taskAvail) {
                scrollOffset_ = 0;
                int y = taskStart + pad;
                for (auto* btn : visibleTask) {
                    btn->rect = { pad, y, w - pad, y + btnH };
                    y += btnH + pad;
                }
            } else {
                scrollNeeded_    = true;
                scrollLeftRect_  = { 0, taskStart,                      w, taskStart + arrowW };
                scrollRightRect_ = { 0, taskStart + taskAvail - arrowW, w, taskStart + taskAvail };
                int inner    = taskAvail - 2 * arrowW - 2 * pad;
                int visCount = std::max(1, (inner + pad) / (btnH + pad));
                maxScrollOffset_ = std::max(0, tCount - visCount);
                scrollOffset_    = std::min(scrollOffset_, maxScrollOffset_);
                for (auto* btn : visibleTask) btn->rect = {};
                int y = taskStart + arrowW + pad;
                for (int i = scrollOffset_; i < scrollOffset_ + visCount && i < tCount; ++i) {
                    visibleTask[i]->rect = { pad, y, w - pad, y + btnH };
                    y += btnH + pad;
                }
            }
        } else {
            for (auto& btn : taskBtns) btn.rect = {};
        }
    }
}

int TaskbarWindow::HitTestButton(POINT pt) const
{
    // Check pinned buttons first
    for (int i = 0; std::cmp_less(i, pinnedButtons_.size()); ++i) {
        if (pinnedButtons_[i].HitTest(pt)) return i;
    }
    // Then task buttons (combined index offset by pinned count)
    const auto& taskBtns = tracker_.Buttons();
    int offset = (int)pinnedButtons_.size();
    for (int i = 0; std::cmp_less(i, taskBtns.size()); ++i) {
        if (taskBtns[i].HitTest(pt)) return offset + i;
    }
    return -1;
}

void TaskbarWindow::LaunchApp(const wchar_t* exe, const wchar_t* args, int nShow)
{
    if (settings_.openAppsOnSameMonitor && hMonitor_) {
        LaunchOnMonitor(hInst_, hMonitor_, exe, args, nShow);
        return;
    }
    ShellExecuteW(nullptr, L"open", exe, args, nullptr, nShow);
}

void TaskbarWindow::ActivateButton(int combinedIdx)
{
    if (combinedIdx < 0 || combinedIdx >= TotalCount()) return;

    if (IsPinnedIdx(combinedIdx)) {
        // Pinned button: always spawn a new window
        const TaskButton& btn = pinnedButtons_[combinedIdx];
        if (!btn.exePath.empty())
            LaunchApp(btn.exePath.c_str());
        return;
    }

    // Task button
    auto& buttons = tracker_.MutableButtons();
    int taskIdx = combinedIdx - (int)pinnedButtons_.size();
    if (taskIdx < 0 || std::cmp_greater_equal(taskIdx, buttons.size())) return;

    TaskButton& btn = buttons[taskIdx];

    if (!btn.IsRunning()) {
        if (!btn.exePath.empty())
            LaunchApp(btn.exePath.c_str());
        return;
    }

    // Minimize only if the window is already visible and in the foreground.
    if (!IsIconic(btn.hwnd) && GetForegroundWindow() == btn.hwnd) {
        ShowWindow(btn.hwnd, SW_MINIMIZE);
        return;
    }

    if (IsIconic(btn.hwnd))
        ShowWindow(btn.hwnd, SW_RESTORE);

    DWORD ourTid    = GetCurrentThreadId();
    DWORD targetTid = GetWindowThreadProcessId(btn.hwnd, nullptr);

    if (ourTid != targetTid)
        AttachThreadInput(ourTid, targetTid, TRUE);

    SetForegroundWindow(btn.hwnd);
    BringWindowToTop(btn.hwnd);

    if (ourTid != targetTid)
        AttachThreadInput(ourTid, targetTid, FALSE);
}

void TaskbarWindow::ShowButtonMenu(int combinedIdx, POINT ptScreen)
{
    if (combinedIdx < 0 || combinedIdx >= TotalCount()) return;

    if (IsPinnedIdx(combinedIdx)) {
        // --- Pinned button menu ---
        const std::wstring exePath = pinnedButtons_[combinedIdx].exePath;
        HWND runningHwnd = FindHwndByExePath(exePath);

        // Query jump list items (works via destination files even if app is not running)
        std::vector<JumpListItem> jumpItems = GetJumpListItems(runningHwnd, exePath);

        std::vector<MenuItem> items;

        // Add jump list items if available (grouped by category)
        if (!jumpItems.empty()) {
            std::wstring header = jumpItems[0].category.empty() ? L"Tasks" : jumpItems[0].category;
            items.push_back({ header, 0, false, false, false, true });
            for (int i = 0; i < static_cast<int>(jumpItems.size()); ++i) {
                UINT id = IDM_JUMPLIST_BASE + static_cast<UINT>(i);
                if (id > IDM_JUMPLIST_MAX) break;
                items.push_back({ jumpItems[i].displayName, id, false, false, false, false });
            }
            items.push_back({ L"", 0, true, false, false, false });
        }

        items.push_back({ L"Open new window",    IDM_OPEN_NEW_WINDOW, false, false, exePath.empty(), false });
        items.push_back({ L"",                   0,                   true,  false, false, false });
        items.push_back({ L"Unpin from taskbar", IDM_PIN_UNPIN,       false, true,  false, false });
        items.push_back({ L"Close window",       IDM_CLOSE_WINDOW,    false, false, !runningHwnd, false });

        UINT id = PopupMenu::Show(hwnd_, ptScreen, std::move(items), colors_, dpi_);

        if (id >= IDM_JUMPLIST_BASE && id <= IDM_JUMPLIST_MAX) {
            int idx = static_cast<int>(id - IDM_JUMPLIST_BASE);
            if (idx < static_cast<int>(jumpItems.size())) {
                const auto& ji = jumpItems[idx];
                const wchar_t* verb = ji.isFolder ? L"explore" : L"open";
                ShellExecuteW(nullptr, verb, ji.path.c_str(),
                              ji.arguments.empty() ? nullptr : ji.arguments.c_str(),
                              ji.workingDir.empty() ? nullptr : ji.workingDir.c_str(),
                              ji.showCmd);
            }
            return;
        }

        switch (id) {
        case IDM_OPEN_NEW_WINDOW:
            if (!exePath.empty())
                LaunchApp(exePath.c_str());
            break;

        case IDM_PIN_UNPIN: {
            Settings updated = settings_;
            if (settings_.pinnedAppsPerMonitor && !monitorDeviceName_.empty()) {
                auto it = updated.pinnedExePathsPerMonitor.find(monitorDeviceName_);
                if (it != updated.pinnedExePathsPerMonitor.end()) {
                    auto& vec = it->second;
                    vec.erase(std::remove_if(vec.begin(), vec.end(),
                        [&](const auto& p) { return _wcsicmp(p.c_str(), exePath.c_str()) == 0; }),
                        vec.end());
                }
            } else {
                updated.pinnedExePaths.clear();
                for (const auto& p : settings_.pinnedExePaths)
                    if (_wcsicmp(p.c_str(), exePath.c_str()) != 0)
                        updated.pinnedExePaths.push_back(p);
            }
            ApplySettings(updated);
            App::Instance().PropagateSettings(settings_, this);
            break;
        }

        case IDM_CLOSE_WINDOW:
            if (runningHwnd)
                PostMessage(runningHwnd, WM_CLOSE, 0, 0);
            break;
        default: break;
        }
        return;
    }

    // --- Task button menu ---
    const auto& taskBtns = tracker_.Buttons();
    int taskIdx = combinedIdx - (int)pinnedButtons_.size();
    if (taskIdx < 0 || std::cmp_greater_equal(taskIdx, taskBtns.size())) return;

    // Copy values before modal message loop can invalidate the reference
    const std::wstring exePath  = taskBtns[taskIdx].exePath;
    const HWND        btnHwnd  = taskBtns[taskIdx].hwnd;
    bool isRunning = taskBtns[taskIdx].IsRunning();

    // Check if this running app is also pinned on this taskbar
    bool isPinned = false;
    if (settings_.pinnedAppsPerMonitor && !monitorDeviceName_.empty()) {
        auto it = settings_.pinnedExePathsPerMonitor.find(monitorDeviceName_);
        if (it != settings_.pinnedExePathsPerMonitor.end()) {
            for (const auto& p : it->second)
                if (!exePath.empty() && _wcsicmp(p.c_str(), exePath.c_str()) == 0) { isPinned = true; break; }
        }
    } else {
        for (const auto& p : settings_.pinnedExePaths)
            if (!exePath.empty() && _wcsicmp(p.c_str(), exePath.c_str()) == 0) { isPinned = true; break; }
    }

    // Query jump list items (works via destination files even if app is not running)
    std::vector<JumpListItem> jumpItems = GetJumpListItems(btnHwnd, exePath);

    std::vector<MenuItem> items;

    // Add jump list items if available (grouped by category)
    if (!jumpItems.empty()) {
        std::wstring header = jumpItems[0].category.empty() ? L"Tasks" : jumpItems[0].category;
        items.push_back({ header, 0, false, false, false, true });
        for (int i = 0; i < static_cast<int>(jumpItems.size()); ++i) {
            UINT jid = IDM_JUMPLIST_BASE + static_cast<UINT>(i);
            if (jid > IDM_JUMPLIST_MAX) break;
            items.push_back({ jumpItems[i].displayName, jid, false, false, false, false });
        }
        items.push_back({ L"", 0, true, false, false, false });
    }

    items.push_back({ L"Open new window",       IDM_OPEN_NEW_WINDOW, false, false, exePath.empty(), false });
    items.push_back({ L"",                      0,                   true,  false, false, false });
    items.push_back({ isPinned ? L"Unpin from taskbar" : L"Pin to taskbar",
                                                IDM_PIN_UNPIN,       false, isPinned, false, false });
    items.push_back({ L"Close window",          IDM_CLOSE_WINDOW,    false, false, !isRunning, false });

    UINT id = PopupMenu::Show(hwnd_, ptScreen, std::move(items), colors_, dpi_);

    if (id >= IDM_JUMPLIST_BASE && id <= IDM_JUMPLIST_MAX) {
        int idx = static_cast<int>(id - IDM_JUMPLIST_BASE);
        if (idx < static_cast<int>(jumpItems.size())) {
            const auto& ji = jumpItems[idx];
            const wchar_t* verb = ji.isFolder ? L"explore" : L"open";
            ShellExecuteW(nullptr, verb, ji.path.c_str(),
                          ji.arguments.empty() ? nullptr : ji.arguments.c_str(),
                          ji.workingDir.empty() ? nullptr : ji.workingDir.c_str(),
                          ji.showCmd);
        }
        return;
    }

    switch (id) {
    case IDM_OPEN_NEW_WINDOW:
        if (!exePath.empty())
            LaunchApp(exePath.c_str());
        break;

    case IDM_PIN_UNPIN: {
        Settings updated = settings_;
        if (settings_.pinnedAppsPerMonitor && !monitorDeviceName_.empty()) {
            auto& vec = updated.pinnedExePathsPerMonitor[monitorDeviceName_];
            if (!isPinned) {
                if (!exePath.empty()) vec.push_back(exePath);
            } else {
                vec.erase(std::remove_if(vec.begin(), vec.end(),
                    [&](const auto& p) { return _wcsicmp(p.c_str(), exePath.c_str()) == 0; }),
                    vec.end());
            }
        } else {
            if (!isPinned) {
                if (!exePath.empty()) updated.pinnedExePaths.push_back(exePath);
            } else {
                updated.pinnedExePaths.erase(std::remove_if(updated.pinnedExePaths.begin(), updated.pinnedExePaths.end(),
                    [&](const auto& p) { return _wcsicmp(p.c_str(), exePath.c_str()) == 0; }),
                    updated.pinnedExePaths.end());
            }
        }
        ApplySettings(updated);
        App::Instance().PropagateSettings(settings_, this);
        break;
    }

    case IDM_CLOSE_WINDOW:
        if (btnHwnd && isRunning)
            PostMessage(btnHwnd, WM_CLOSE, 0, 0);
        break;
    default: break;
    }
}

void TaskbarWindow::ShowBackgroundMenu(POINT ptScreen)
{
    std::vector<MenuItem> items = {
        { L"Settings...",         IDM_SETTINGS,          false, false, false },
        { L"Export settings...",  IDM_EXPORT_SETTINGS,   false, false, false },
        { L"",                    0,                     true,  false, false },
        { L"Rebuild icon cache",  IDM_REBUILD_ICON_CACHE, false, false, false },
        { L"About Winzoo...",     IDM_ABOUT,             false, false, false },
        { L"",                    0,                     true,  false, false },
        { L"Restart",             IDM_RESTART,           false, false, false },
        { L"Close",               IDM_CLOSE_TASKBAR,     false, false, false },
    };

    UINT id = PopupMenu::Show(hwnd_, ptScreen, std::move(items), colors_, dpi_);

    switch (id) {
    case IDM_SETTINGS:
        if (SettingsDialog::Show(hwnd_, settings_)) {
            App::Instance().PropagateSettings(settings_, this);
            ApplySettings(settings_);
        }
        break;

    case IDM_EXPORT_SETTINGS:
        if (ExportSettingsToFile(settings_))
            MessageBoxW(hwnd_,
                        L"Settings exported to winzoo-settings.json in the application folder.",
                        L"Export successful", MB_OK | MB_ICONINFORMATION);
        else
            MessageBoxW(hwnd_,
                        L"Failed to write winzoo-settings.json.\n"
                        L"Check that the application folder is writable.",
                        L"Export failed", MB_OK | MB_ICONERROR);
        break;

    case IDM_ABOUT:
        MessageBoxW(hwnd_,
                    L"Winzoo v0.1\n\nA lightweight taskbar replacement for Windows 10/11.",
                    L"About Winzoo",
                    MB_OK | MB_ICONINFORMATION);
        break;

    case IDM_REBUILD_ICON_CACHE:
        appIconCache_.Clear();
        StartIconLoadThread();
        break;

    case IDM_RESTART:
        App::Instance().RequestRestart();
        break;

    case IDM_CLOSE_TASKBAR:
        PostQuitMessage(0);
        break;
    default: break;
    }
}

void TaskbarWindow::ShowStatusIconMenu(int which, POINT ptScreen)
{
    if (which == 1) {   // Volume
        std::vector<MenuItem> items = {
            { L"Open Volume mixer", IDM_VOL_MIXER,    false, false, false },
            { L"Sounds",            IDM_VOL_SOUNDS,   false, false, false },
            { L"Sound settings",    IDM_VOL_SETTINGS, false, false, false },
        };
        UINT id = PopupMenu::Show(hwnd_, ptScreen, std::move(items), colors_, dpi_);
        switch (id) {
        case IDM_VOL_MIXER:
            LaunchApp(L"sndvol.exe");
            break;
        case IDM_VOL_SOUNDS:
            LaunchApp(L"mmsys.cpl");
            break;
        case IDM_VOL_SETTINGS:
            LaunchApp(L"ms-settings:sound");
            break;
        default: break;
        }
    } else if (which == 2) {   // Network
        std::vector<MenuItem> items = {
            { L"Open Network & Internet settings",  IDM_NET_SETTINGS, false, false, false },
            { L"Open Network and Sharing Center",   IDM_NET_SHARING,  false, false, false },
        };
        UINT id = PopupMenu::Show(hwnd_, ptScreen, std::move(items), colors_, dpi_);
        switch (id) {
        case IDM_NET_SETTINGS:
            LaunchApp(L"ms-settings:network-status");
            break;
        case IDM_NET_SHARING:
            LaunchApp(L"control.exe", L"/name Microsoft.NetworkAndSharingCenter");
            break;
        default: break;
        }
    } else if (which == 3) {   // Battery
        std::vector<MenuItem> items = {
            { L"Adjust screen brightness", IDM_BAT_BRIGHTNESS, false, false, false },
            { L"Power Options",            IDM_BAT_POWER,      false, false, false },
            { L"Battery settings",         IDM_BAT_SETTINGS,   false, false, false },
        };
        UINT id = PopupMenu::Show(hwnd_, ptScreen, std::move(items), colors_, dpi_);
        switch (id) {
        case IDM_BAT_BRIGHTNESS:
            LaunchApp(L"ms-settings:display");
            break;
        case IDM_BAT_POWER:
            LaunchApp(L"powercfg.cpl");
            break;
        case IDM_BAT_SETTINGS:
            LaunchApp(L"ms-settings:batterysaver");
            break;
        default: break;
        }
    }
}

void TaskbarWindow::ShowLangMenu(POINT ptScreen)
{
    // Enumerate all installed keyboard layouts
    int count = GetKeyboardLayoutList(0, nullptr);
    if (count <= 0) return;
    std::vector<HKL> hkls(static_cast<size_t>(count));
    count = GetKeyboardLayoutList(count, hkls.data());

    std::vector<MenuItem> items;
    std::vector<HKL>      hklForItem;   // parallel to items, tracks which HKL each item maps to

    for (int i = 0; i < count; ++i) {
        LANGID langId = static_cast<LANGID>(
            reinterpret_cast<ULONG_PTR>(hkls[i]) & 0xFFFF);
        std::wstring text = GetLangDisplayText(langId);
        if (text.empty()) continue;
        UINT id = IDM_LANG_BASE + static_cast<UINT>(hklForItem.size());
        if (id > IDM_LANG_MAX) break;
        bool isCurrent = (hkls[i] == currentHkl_);
        items.push_back({ text, id, false, isCurrent, false });
        hklForItem.push_back(hkls[i]);
    }

    if (!items.empty())
        items.push_back({ {}, 0, true });   // separator before settings link
    items.push_back({ L"Language settings", IDM_LANG_SETTINGS, false, false, false });

    UINT selected = PopupMenu::Show(hwnd_, ptScreen, std::move(items), colors_, dpi_);

    if (selected == IDM_LANG_SETTINGS) {
        LaunchApp(L"ms-settings:regionlanguage");
        return;
    }
    if (selected >= IDM_LANG_BASE) {
        size_t idx = selected - IDM_LANG_BASE;
        if (idx < hklForItem.size()) {
            HKL targetHkl = hklForItem[idx];
            HWND fg = GetForegroundWindow();
            if (fg)
                PostMessage(fg, WM_INPUTLANGCHANGEREQUEST, 0,
                            reinterpret_cast<LPARAM>(targetHkl));
        }
    }
}

RECT TaskbarWindow::GetStartBtnScreenRect() const
{
    RECT r = startBtnRect_;
    POINT tl = { r.left, r.top };
    POINT br = { r.right, r.bottom };
    ClientToScreen(hwnd_, &tl);
    ClientToScreen(hwnd_, &br);
    return { tl.x, tl.y, br.x, br.y };
}

void TaskbarWindow::ShowAppMenu()
{
    if (appEntries_.empty()) return;
    if (menuOpen_) return;   // Guard against re-entrant calls from within the nested loop
    menuOpen_ = true;
    RECT btnScreen = GetStartBtnScreenRect();
    AppMenuWindow::Show(hwnd_, btnScreen, settings_.position,
                        appEntries_,    // copied by value into menu
                        settings_, colors_, dpi_);
    menuOpen_ = false;
    menuLastClosedTick_ = GetTickCount();
}

void TaskbarWindow::StartScanThread(bool isFirstScan)
{
    HWND hwnd = hwnd_;
    unsigned gen = ++scanGen_;
    WPARAM wp = MAKEWPARAM(isFirstScan ? 0 : 1, static_cast<WORD>(gen));
    std::thread([hwnd, wp]() {
        auto pEntries = std::make_unique<std::vector<AppEntry>>(AppScanner::Scan());
        if (!PostMessageW(hwnd, WM_APP_SCAN_DONE, wp,
                          reinterpret_cast<LPARAM>(pEntries.get())))
            return;  // on failure, unique_ptr auto-deletes
        pEntries.release();  // ownership transferred to WM_APP_SCAN_DONE handler
    }).detach();
}

void TaskbarWindow::StartIconLoadThread()
{
    HWND hwnd = hwnd_;
    int  sizePx = Scale(48, dpi_);
    unsigned gen = ++iconGen_;

    // Determine which pinned paths this taskbar shows
    static const std::vector<std::wstring> kEmptyPinned;
    const std::vector<std::wstring>* pinnedPaths = nullptr;
    if (settings_.pinnedAppsPerMonitor && !monitorDeviceName_.empty()) {
        auto it = settings_.pinnedExePathsPerMonitor.find(monitorDeviceName_);
        pinnedPaths = (it != settings_.pinnedExePathsPerMonitor.end()) ? &it->second : &kEmptyPinned;
    } else {
        pinnedPaths = &settings_.pinnedExePaths;
    }

    std::vector<std::wstring> paths;
    paths.reserve(appEntries_.size() + pinnedPaths->size());
    for (const auto& e : appEntries_) {
        paths.push_back(e.iconPath.empty() ? e.exePath : e.iconPath);
    }
    // Also include pinned exe paths (may not be in the scanned app list)
    for (const auto& p : *pinnedPaths) {
        bool alreadyIn = false;
        for (const auto& ex : paths)
            if (_wcsicmp(ex.c_str(), p.c_str()) == 0) { alreadyIn = true; break; }
        if (!alreadyIn)
            paths.push_back(p);
    }
    WPARAM wp = MAKEWPARAM(static_cast<WORD>(sizePx), static_cast<WORD>(gen));
    std::thread([hwnd, wp, paths = std::move(paths)]() {
        int sizePx = LOWORD(wp);
        using Pair = std::pair<std::wstring, HICON>;
        auto pResults = std::make_unique<std::vector<Pair>>(paths.size());
        for (size_t i = 0; i < paths.size(); ++i)
            (*pResults)[i].first = paths[i];

        // Load icons in parallel: up to 4 workers each grab paths via an atomic counter.
        // MTA avoids STA message-pump blocking; each worker owns its COM apartment.
        const int kWorkers = std::min(static_cast<int>(paths.size()), 4);
        if (kWorkers > 0) {
            std::atomic<int> nextIdx{ 0 };
            std::vector<std::thread> workers;
            workers.reserve(kWorkers);
            for (int w = 0; w < kWorkers; ++w) {
                workers.emplace_back([&paths, pRaw = pResults.get(), &nextIdx, sizePx]() {
                    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    int idx = 0;
                    while ((idx = nextIdx.fetch_add(1)) < static_cast<int>(paths.size()))
                        (*pRaw)[idx].second = AppIconCache::LoadStatic(paths[idx], sizePx);
                    if (SUCCEEDED(hrCom)) CoUninitialize();
                });
            }
            for (auto& t : workers) t.join();
        }

        if (!PostMessageW(hwnd, WM_APP_ICONS_DONE, wp,
                          reinterpret_cast<LPARAM>(pResults.get()))) {
            for (auto& [p, icon] : *pResults)
                if (icon) DestroyIcon(icon);
            return;  // unique_ptr auto-deletes
        }
        pResults.release();  // ownership transferred to WM_APP_ICONS_DONE handler
    }).detach();
}

LRESULT TaskbarWindow::HandleMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    // Dynamic message registration checks
    if (shellHookMsg_ && uMsg == shellHookMsg_) {
        tracker_.OnShellMessage(wParam, lParam);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    if (progressRelayMsg_ && uMsg == progressRelayMsg_) {
        HWND appHwnd = TaskbarProxy::DecodeHwnd(wParam);
        int  state   = TaskbarProxy::DecodeState(lParam);
        int  percent = TaskbarProxy::DecodePercent(lParam);
        // Walk up to root in case the app passed a child HWND
        HWND rootHwnd = GetAncestor(appHwnd, GA_ROOT);
        for (auto& btn : tracker_.MutableButtons()) {
            if (btn.hwnd == appHwnd || btn.hwnd == rootHwnd) {
                btn.progress.state            = state;
                btn.progress.percent          = percent;
                btn.progress.lastProgressTick = (state != kTBPF_NOPROGRESS) ? GetTickCount() : 0;
                InvalidateRect(hwnd, nullptr, FALSE);
                break;
            }
        }
        return 0;
    }
    if (taskbarCreatedMsg_ && uMsg == taskbarCreatedMsg_) {
        HWND tray = FindWindow(L"Shell_TrayWnd", nullptr);
        // If FindWindow returns our own proxy window, look past it for Explorer's.
        if (tray) {
            DWORD pid = 0;
            GetWindowThreadProcessId(tray, &pid);
            if (pid == GetCurrentProcessId())
                tray = FindWindowEx(nullptr, tray, L"Shell_TrayWnd", nullptr);
        }
        if (tray) ShowWindow(tray, SW_HIDE);
        appBar_.Unregister();
        appBar_.Register(hwnd_, settings_.position, EffectiveThicknessPx());
        // For floating mode, AppBar is not registered so GetReservedRect() returns {0,0,0,0};
        // use CalculateWindowRect() instead to preserve the floating position.
        RECT rc = (settings_.position == TaskbarPosition::Floating)
                ? CalculateWindowRect()
                : appBar_.GetReservedRect();
        SetWindowPos(hwnd_, HWND_TOPMOST,
                     rc.left, rc.top,
                     rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
        return 0;
    }
    if (appBarCallbackMsg_ && uMsg == appBarCallbackMsg_) {
        appBar_.OnCallback(wParam, lParam);
        return 0;
    }

    switch (uMsg) {
    case WM_CREATE: {
        dpi_ = GetWindowDpi(hwnd);
        RECT client;
        GetClientRect(hwnd, &client);
        HDC hdc = GetDC(hwnd);
        renderer_.Resize(client.right, client.bottom, hdc);
        ReleaseDC(hwnd, hdc);

        if (settings_.position != TaskbarPosition::Floating)
            appBar_.Register(hwnd_, settings_.position, EffectiveThicknessPx());

        shellHookMsg_ = tracker_.ShellHookMessage();
        if (shellHookMsg_ == 0)
            shellHookMsg_ = RegisterWindowMessage(L"SHELLHOOK");
        tracker_.Initialize(hwnd_, &iconCache_,
                            [this]() {
                                LayoutButtons();
                                InvalidateRect(hwnd_, nullptr, FALSE);
                            });

        RebuildPinnedButtons();
        LayoutButtons();
        SetTimer(hwnd, kTimerActiveWindow, kTimerIntervalMs, nullptr);
        SetTimer(hwnd, kTimerAppScanFirst, 500, nullptr);
        SetTimer(hwnd, kTimerStatus, kTimerStatusMs, nullptr);
        SetTimer(hwnd, kTimerTray, kTimerTrayMs, nullptr);
        // Immediately prime status/lang data so layout includes them on first paint
        statusData_ = PollSystemStatus();
        auto [lt, lh]    = GetCurrentInputLanguage();
        currentLangText_ = lt;
        currentHkl_      = lh;
        RefreshTrayIcons();
        LayoutButtons();

        // Register relay message on all windows; install proxy (COM registration) on primary only.
        progressRelayMsg_ = RegisterWindowMessageW(L"WinzooProgress");
        if (isPrimary_)
            proxy_.Install(hwnd_);

        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT client;
        GetClientRect(hwnd, &client);

        POINT ghostPt = drag_.CurrentPoint();
        ScreenToClient(hwnd, &ghostPt);

        ClockInfo clock;
        if (settings_.showClock) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            clock.visible  = true;
            clock.rect     = clockRect_;
            clock.timeLine = ApplyClockPattern(settings_.clockTimeFormat, st, true);
            clock.dateLine = ApplyClockPattern(settings_.clockDateFormat, st, false);
            clock.timeFontPt = fittedTimePt_;
            clock.dateFontPt = fittedDatePt_;
            clock.lineSpacing = settings_.clockLineSpacing;
            clock.timeColor  = settings_.clockTimeColor;
            clock.dateColor  = settings_.clockDateColor;
        }

        bool isHoriz = (settings_.position != TaskbarPosition::Left &&
                        settings_.position != TaskbarPosition::Right);
        ScrollInfo scroll;
        scroll.needed   = scrollNeeded_;
        scroll.isHoriz  = isHoriz;
        scroll.leftRect  = scrollLeftRect_;
        scroll.rightRect = scrollRightRect_;
        scroll.canLeft   = scrollOffset_ > 0;
        scroll.canRight  = scrollOffset_ < maxScrollOffset_;
        scroll.hovered   = hoveredScroll_;

        // Build combined buttons vector for rendering (pinned first, then task)
        std::vector<TaskButton> allButtons;
        allButtons.reserve(pinnedButtons_.size() + tracker_.Buttons().size());
        for (auto b : pinnedButtons_)     { b.iconDrawSz = settings_.appButtonIconSize; allButtons.push_back(std::move(b)); }
        for (auto b : tracker_.Buttons()) { b.iconDrawSz = settings_.appButtonIconSize; allButtons.push_back(std::move(b)); }

        bool showLangInStatus = settings_.showLangIndicator && isHoriz && !currentLangText_.empty();
        StatusZoneInfo status;
        status.visible = isHoriz && (settings_.showStatusZone || showLangInStatus);
        if (status.visible) {
            status.rect = statusZoneRect_;
            if (settings_.showStatusZone) {
                status.volAvailable = statusData_.volAvailable && !settings_.hideDefaultTrayIcons;
                status.volRect      = volIconRect_;
                status.volLevel     = statusData_.volLevel;
                status.volMuted     = statusData_.volMuted;
                status.volHovered   = (hoveredStatus_ == 1);
                status.netAvailable = statusData_.netAvailable && !settings_.hideDefaultTrayIcons;
                status.netRect      = netIconRect_;
                status.netConnected = statusData_.netConnected;
                status.netHovered   = (hoveredStatus_ == 2);
                status.batAvailable = statusData_.batAvailable;
                status.batRect      = batIconRect_;
                status.batOnAC      = statusData_.batOnAC;
                status.batCharging  = statusData_.batCharging;
                status.batPercent   = statusData_.batPercent;
                status.batHovered   = (hoveredStatus_ == 3);
            }
            if (showLangInStatus) {
                status.langAvailable = true;
                status.langRect      = langIconRect_;
                status.langText      = currentLangText_;
                status.langHovered   = (hoveredStatus_ == 4);
            }
        }

        TrayZoneInfo tray;
        tray.visible = settings_.showTrayIcons && isHoriz && !trayIcons_.empty();
        if (tray.visible) {
            int n = static_cast<int>(trayIcons_.size());
            tray.icons.resize(n);
            for (int i = 0; i < n; ++i) {
                tray.icons[i].hIcon   = trayIcons_[i].hIcon;
                tray.icons[i].rect    = std::cmp_less(i, trayIconRects_.size()) ? trayIconRects_[i] : RECT{};
                tray.icons[i].hovered = (i == hoveredTrayIdx_);
            }
            if (trayDragging_) {
                tray.dragGhostIdx = trayDragStart_;
                tray.ghostPt      = trayDragPt_;
            }
        }

        renderer_.Paint(hdc, client.right, client.bottom,
                        allButtons,
                        hoveredIdx_,
                        drag_.State() == DragState::Pressed  ? drag_.DragIndex() : -1,
                        drag_.State() == DragState::Dragging ? drag_.DragIndex() : -1,
                        ghostPt,
                        colors_, dpi_, clock, scroll,
                        StartButtonInfo{ showStartButton_, startBtnRect_, hoveredStart_ },
                        MinimizedIndicatorOptions{
                            settings_.showMinimizedIndicator,
                            settings_.minimizedIndicatorType,
                            settings_.minimizedIndicatorW,
                            settings_.minimizedIndicatorH
                        },
                        pinnedSepX_,
                        status,
                        tray,
                        ProgressBarOptions{
                            settings_.showProgressBars,
                            [&]() -> COLORREF {
                                if (!settings_.progressBarUseThemeColor)
                                    return settings_.progressBarColor;
                                COLORREF a = colors_.buttonActive;
                                return RGB(
                                    std::min(255, static_cast<int>(GetRValue(a)) + 100),
                                    std::min(255, static_cast<int>(GetGValue(a)) + 100),
                                    std::min(255, static_cast<int>(GetBValue(a)) + 100));
                            }(),
                            settings_.progressBarHeight
                        },
                        settings_.buttonOutlineRadius,
                        settings_.showPinnedAppsAsButtons,
                        settings_.showSeparators,
                        settings_.separatorColor);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_NCHITTEST: {
        LRESULT def = DefWindowProc(hwnd, uMsg, wParam, lParam);
        if (settings_.position != TaskbarPosition::Floating)
            return def;

        POINT clientPt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &clientPt);

        RECT crc;
        GetClientRect(hwnd, &crc);
        int b = Scale(6, dpi_);

        if (clientPt.x < b)               return HTLEFT;
        if (clientPt.x >= crc.right - b)  return HTRIGHT;

        if (HitTestButton(clientPt) >= 0)                                   return HTCLIENT;
        if (PtInRect(&startBtnRect_, clientPt))                             return HTCLIENT;
        if (settings_.showLangIndicator && !currentLangText_.empty()
            && PtInRect(&langIconRect_, clientPt))                          return HTCLIENT;
        if (scrollNeeded_ && (PtInRect(&scrollLeftRect_,  clientPt)
                           || PtInRect(&scrollRightRect_, clientPt)))       return HTCLIENT;
        if (settings_.showTrayIcons) {
            for (auto& r : trayIconRects_)
                if (PtInRect(&r, clientPt)) return HTCLIENT;
        }
        if (settings_.showStatusZone) {
            if (PtInRect(&volIconRect_, clientPt) ||
                PtInRect(&netIconRect_, clientPt) ||
                PtInRect(&batIconRect_, clientPt))  return HTCLIENT;
        }
        return HTCAPTION;
    }

    case WM_EXITSIZEMOVE: {
        if (settings_.position == TaskbarPosition::Floating) {
            RECT wrc;
            GetWindowRect(hwnd, &wrc);
            settings_.floatX     = wrc.left;
            settings_.floatY     = wrc.top;
            settings_.floatWidth = (wrc.right - wrc.left) * 96 / dpi_;
            SaveSettings(settings_);
        }
        return 0;
    }

    case WM_SIZE: {
        int w = LOWORD(lParam);
        int h = HIWORD(lParam);
        if (w > 0 && h > 0) {
            HDC hdc = GetDC(hwnd);
            renderer_.Resize(w, h, hdc);
            ReleaseDC(hwnd, hdc);
            LayoutButtons();
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int idx = HitTestButton(pt);
        if (idx != hoveredIdx_) {
            hoveredIdx_ = idx;
            InvalidateRect(hwnd, nullptr, FALSE);
        }

        bool newHovStart = (PtInRect(&startBtnRect_, pt) != FALSE);
        if (newHovStart != hoveredStart_) {
            hoveredStart_ = newHovStart;
            InvalidateRect(hwnd, nullptr, FALSE);
        }

        int newHovScroll = 0;
        if (scrollNeeded_) {
            if (PtInRect(&scrollLeftRect_,  pt)) newHovScroll = 1;
            if (PtInRect(&scrollRightRect_, pt)) newHovScroll = 2;
        }
        if (newHovScroll != hoveredScroll_) {
            hoveredScroll_ = newHovScroll;
            InvalidateRect(hwnd, nullptr, FALSE);
        }

        int newHovStatus = 0;
        if (settings_.showStatusZone) {
            if (statusData_.volAvailable && !settings_.hideDefaultTrayIcons && PtInRect(&volIconRect_, pt)) newHovStatus = 1;
            else if (statusData_.netAvailable && !settings_.hideDefaultTrayIcons && PtInRect(&netIconRect_, pt)) newHovStatus = 2;
            else if (statusData_.batAvailable && PtInRect(&batIconRect_, pt)) newHovStatus = 3;
        }
        if (newHovStatus == 0 && settings_.showLangIndicator &&
            !currentLangText_.empty() && PtInRect(&langIconRect_, pt))
            newHovStatus = 4;
        if (newHovStatus != hoveredStatus_) {
            hoveredStatus_ = newHovStatus;
            InvalidateRect(hwnd, nullptr, FALSE);
        }

        // Tray icon hover
        int newHovTray = -1;
        if (settings_.showTrayIcons) {
            for (int i = 0; std::cmp_less(i, trayIconRects_.size()); ++i) {
                if (PtInRect(&trayIconRects_[i], pt)) { newHovTray = i; break; }
            }
        }
        if (newHovTray != hoveredTrayIdx_) {
            hoveredTrayIdx_ = newHovTray;
            InvalidateRect(hwnd, nullptr, FALSE);
        }

        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);

        if (drag_.State() != DragState::Idle) {
            POINT screenPt = pt;
            ClientToScreen(hwnd, &screenPt);
            drag_.OnMouseMove(screenPt);
            InvalidateRect(hwnd, nullptr, FALSE);
        }

        // Tray drag threshold
        if (trayDragStart_ >= 0 && !trayDragging_) {
            constexpr int kThresh = 6;
            if (std::abs(pt.x - trayDragPt_.x) > kThresh ||
                std::abs(pt.y - trayDragPt_.y) > kThresh)
            {
                trayDragging_ = true;
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        if (trayDragging_) {
            trayDragPt_ = pt;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        hoveredIdx_    = -1;
        hoveredScroll_ = 0;
        hoveredStart_  = false;
        hoveredStatus_ = 0;
        hoveredTrayIdx_= -1;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

        // Start button takes priority
        if (PtInRect(&startBtnRect_, pt)) {
            if (!menuOpen_ && GetTickCount() - menuLastClosedTick_ > 200)
                ShowAppMenu();
            return 0;
        }

        if (scrollNeeded_) {
            if (PtInRect(&scrollLeftRect_, pt)) {
                if (scrollOffset_ > 0) { --scrollOffset_; LayoutButtons(); InvalidateRect(hwnd, nullptr, FALSE); }
                return 0;
            }
            if (PtInRect(&scrollRightRect_, pt)) {
                if (scrollOffset_ < maxScrollOffset_) { ++scrollOffset_; LayoutButtons(); InvalidateRect(hwnd, nullptr, FALSE); }
                return 0;
            }
        }

        // Lang indicator — left click opens language switcher
        if (settings_.showLangIndicator && !currentLangText_.empty() &&
            PtInRect(&langIconRect_, pt))
        {
            POINT screenPt = pt;
            ClientToScreen(hwnd, &screenPt);
            ShowLangMenu(screenPt);
            return 0;
        }

        // Tray icon drag/click start
        if (settings_.showTrayIcons) {
            for (int i = 0; std::cmp_less(i, trayIconRects_.size()); ++i) {
                if (PtInRect(&trayIconRects_[i], pt)) {
                    trayDragStart_ = i;
                    trayDragPt_    = pt;
                    trayDragging_  = false;
                    return 0;
                }
            }
        }

        int idx = HitTestButton(pt);
        if (idx >= 0) {
            POINT screenPt = pt;
            ClientToScreen(hwnd, &screenPt);
            drag_.OnButtonDown(idx, screenPt);
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        bool wasDragging = drag_.State() == DragState::Dragging;
        bool wasPressed  = drag_.State() == DragState::Pressed;
        int  origIdx     = drag_.DragIndex();

        // Handle tray drag/click first
        if (trayDragStart_ >= 0) {
            if (trayDragging_) {
                // Find drop target icon slot
                int dropIdx = -1;
                for (int i = 0; std::cmp_less(i, trayIconRects_.size()); ++i) {
                    RECT r = trayIconRects_[i];
                    int cx = (r.left + r.right) / 2;
                    if (pt.x < cx) { dropIdx = i; break; }
                }
                if (dropIdx < 0) dropIdx = static_cast<int>(trayIconRects_.size()) - 1;
                if (dropIdx < 0) dropIdx = 0;

                int from = trayDragStart_;
                int to   = dropIdx;
                if (from != to && std::cmp_less(from, trayIcons_.size()) &&
                    std::cmp_less(to, trayIcons_.size()))
                {
                    // Reorder trayIcons_ and settings_.trayIconOrder
                    TrayIconEntry moved = std::move(trayIcons_[from]);
                    trayIcons_.erase(trayIcons_.begin() + from);
                    int insertAt = (to > from) ? to - 1 : to;
                    trayIcons_.insert(trayIcons_.begin() + insertAt, std::move(moved));

                    // Rebuild trayIconOrder from new display order
                    settings_.trayIconOrder.clear();
                    for (const auto& e : trayIcons_) settings_.trayIconOrder.push_back(e.orderKey);
                    SaveSettings(settings_);
                    LayoutButtons();
                }
                ReleaseCapture();
            } else {
                // Plain left click — forward notification to the icon's owner
                int i = trayDragStart_;
                if (std::cmp_less(i, trayIcons_.size()) && trayIcons_[i].hWnd &&
                    trayIcons_[i].uCallbackMsg)
                {
                    POINT screenPt = pt;
                    ClientToScreen(hwnd, &screenPt);
                    ForwardTrayNotification(trayIcons_[i].hWnd, trayIcons_[i].uCallbackMsg,
                                            trayIcons_[i].uID, screenPt, /*rightClick=*/false);
                }
            }
            trayDragStart_ = -1;
            trayDragging_  = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (wasDragging) {
            // Perform the drop — only swap within the same zone
            int dropIdx = HitTestButton(pt);
            if (dropIdx >= 0 && dropIdx != origIdx) {
                bool origPinned = IsPinnedIdx(origIdx);
                bool dropPinned = IsPinnedIdx(dropIdx);
                if (origPinned && dropPinned) {
                    // Swap within pinned zone and persist the new order
                    std::swap(pinnedButtons_[origIdx], pinnedButtons_[dropIdx]);
                    if (settings_.pinnedAppsPerMonitor && !monitorDeviceName_.empty()) {
                        auto it = settings_.pinnedExePathsPerMonitor.find(monitorDeviceName_);
                        if (it != settings_.pinnedExePathsPerMonitor.end() &&
                            std::cmp_less(origIdx, it->second.size()) &&
                            std::cmp_less(dropIdx, it->second.size())) {
                            std::swap(it->second[origIdx], it->second[dropIdx]);
                        }
                    } else if (std::cmp_less(origIdx, settings_.pinnedExePaths.size()) &&
                               std::cmp_less(dropIdx, settings_.pinnedExePaths.size())) {
                        std::swap(settings_.pinnedExePaths[origIdx],
                                  settings_.pinnedExePaths[dropIdx]);
                    }
                    SaveSettings(settings_);
                    LayoutButtons();
                } else if (!origPinned && !dropPinned) {
                    // Swap within task zone
                    int pi = origIdx - (int)pinnedButtons_.size();
                    int pj = dropIdx - (int)pinnedButtons_.size();
                    auto& buttons = tracker_.MutableButtons();
                    std::swap(buttons[pi], buttons[pj]);
                    LayoutButtons();
                }
                // Cross-zone drops are silently ignored
            }
        }

        drag_.OnButtonUp();
        ReleaseCapture();

        if (wasPressed) {
            // Check status icon click first
            if (settings_.showStatusZone) {
                if (statusData_.volAvailable && PtInRect(&volIconRect_, pt)) {
                    // Open volume flyout: sndvol.exe -f <hwnd> positions it near our window
                    wchar_t arg[32];
                    swprintf_s(arg, L"-f %Iu", reinterpret_cast<UINT_PTR>(hwnd));
                    LaunchApp(L"sndvol.exe", arg);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (statusData_.netAvailable && PtInRect(&netIconRect_, pt)) {
                    // Open available networks flyout
                    LaunchApp(L"ms-availablenetworks:");
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (statusData_.batAvailable && PtInRect(&batIconRect_, pt)) {
                    LaunchApp(L"ms-settings:batterysaver");
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
            ActivateButton(origIdx);
        }

        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_CAPTURECHANGED:
        drag_.OnCaptureChanged();
        trayDragStart_ = -1;
        trayDragging_  = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_MBUTTONUP: {
        if (settings_.middleClickClose) {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            int idx = HitTestButton(pt);
            // Middle-click close only applies to task buttons, not pinned buttons
            if (idx >= 0 && !IsPinnedIdx(idx)) {
                int taskIdx = idx - (int)pinnedButtons_.size();
                const auto& btns = tracker_.Buttons();
                if (std::cmp_less(taskIdx, btns.size())) {
                    HWND target = btns[taskIdx].hwnd;
                    if (target) PostMessage(target, WM_CLOSE, 0, 0);
                }
            }
        }
        return 0;
    }

    case WM_RBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        POINT screenPt = pt;
        ClientToScreen(hwnd, &screenPt);

        // Tray icon right click — forward notification to the icon's owner
        if (settings_.showTrayIcons) {
            for (int i = 0; std::cmp_less(i, trayIconRects_.size()); ++i) {
                if (PtInRect(&trayIconRects_[i], pt)) {
                    if (std::cmp_less(i, trayIcons_.size()) && trayIcons_[i].hWnd &&
                        trayIcons_[i].uCallbackMsg)
                    {
                        ForwardTrayNotification(trayIcons_[i].hWnd, trayIcons_[i].uCallbackMsg,
                                                trayIcons_[i].uID, screenPt, /*rightClick=*/true);
                    }
                    return 0;
                }
            }
        }

        // Status icons get their own context menus
        if (settings_.showStatusZone) {
            if (statusData_.volAvailable && PtInRect(&volIconRect_, pt)) {
                ShowStatusIconMenu(1, screenPt);
                return 0;
            }
            if (statusData_.netAvailable && PtInRect(&netIconRect_, pt)) {
                ShowStatusIconMenu(2, screenPt);
                return 0;
            }
            if (statusData_.batAvailable && PtInRect(&batIconRect_, pt)) {
                ShowStatusIconMenu(3, screenPt);
                return 0;
            }
        }
        if (settings_.showLangIndicator && !currentLangText_.empty() &&
            PtInRect(&langIconRect_, pt))
        {
            ShowLangMenu(screenPt);
            return 0;
        }

        int idx = HitTestButton(pt);
        if (idx >= 0)
            ShowButtonMenu(idx, screenPt);
        else
            ShowBackgroundMenu(screenPt);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        if (scrollNeeded_) {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (delta > 0 && scrollOffset_ > 0) {
                --scrollOffset_; LayoutButtons(); InvalidateRect(hwnd, nullptr, FALSE);
            } else if (delta < 0 && scrollOffset_ < maxScrollOffset_) {
                ++scrollOffset_; LayoutButtons(); InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }

    case WM_TIMER:
        if (wParam == kTimerActiveWindow) {
            tracker_.UpdateActiveWindow();
            if (settings_.showLangIndicator) {
                auto [newText, newHkl] = GetCurrentInputLanguage();
                if (newText != currentLangText_) {
                    currentLangText_ = newText;
                    currentHkl_      = newHkl;
                    LayoutButtons();
                } else {
                    currentHkl_ = newHkl;
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wParam == kTimerAppScanFirst) {
            KillTimer(hwnd, kTimerAppScanFirst);
            StartScanThread(true);
        } else if (wParam == kTimerAppScan) {
            StartScanThread(false);
        } else if (wParam == kTimerStatus) {
            SystemStatusData fresh = PollSystemStatus();
            bool availChanged = (fresh.volAvailable != statusData_.volAvailable ||
                                 fresh.netAvailable != statusData_.netAvailable ||
                                 fresh.batAvailable != statusData_.batAvailable);
            statusData_ = fresh;
            if (availChanged) LayoutButtons();
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wParam == kTimerTray) {
            RefreshTrayIcons();
        }
        return 0;

    case WM_APP_SCAN_DONE: {
        auto pEntries = std::unique_ptr<std::vector<AppEntry>>(
            reinterpret_cast<std::vector<AppEntry>*>(lParam));
        unsigned gen = HIWORD(wParam);
        if (!shutdownPending_ && gen == scanGen_) {
            int iconSz = Scale(48, dpi_);
            bool isFirst = (LOWORD(wParam) == 0);
            if (isFirst) {
                appEntries_ = std::move(*pEntries);
                SetTimer(hwnd_, kTimerAppScan, kTimerAppScanMs, nullptr);
                StartIconLoadThread();
            } else {
                for (auto& e : *pEntries) {
                    const std::wstring& p = e.iconPath.empty() ? e.exePath : e.iconPath;
                    e.icon = appIconCache_.TryGet(p, iconSz);
                }
                appEntries_ = std::move(*pEntries);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        return 0;
    }

    case WM_APP_ICONS_DONE: {
        auto pIcons = std::unique_ptr<std::vector<std::pair<std::wstring, HICON>>>(
            reinterpret_cast<std::vector<std::pair<std::wstring, HICON>>*>(lParam));
        unsigned gen = HIWORD(wParam);
        if (!shutdownPending_ && gen == iconGen_) {
            int iconSz = LOWORD(wParam);
            for (auto& [path, icon] : *pIcons)
                appIconCache_.Store(path, iconSz, icon);
            if (iconSz == Scale(48, dpi_)) {
                for (auto& e : appEntries_) {
                    const std::wstring& p = e.iconPath.empty() ? e.exePath : e.iconPath;
                    e.icon = appIconCache_.TryGet(p, iconSz);
                }
                for (auto& btn : pinnedButtons_)
                    btn.icon = appIconCache_.TryGet(btn.exePath, iconSz);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        } else {
            for (auto& [path, icon] : *pIcons)
                if (icon) DestroyIcon(icon);
        }
        return 0;
    }

    case WM_APP_SHOW_MENU: {
        POINT pt;
        GetCursorPos(&pt);
        ShowBackgroundMenu(pt);
        return 0;
    }

    case WM_DPICHANGED: {
        dpi_ = HIWORD(wParam);
        auto* prc = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(hwnd_, HWND_TOPMOST,
                     prc->left, prc->top,
                     prc->right - prc->left, prc->bottom - prc->top,
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
        HDC hdc = GetDC(hwnd_);
        renderer_.Resize(prc->right - prc->left, prc->bottom - prc->top, hdc);
        ReleaseDC(hwnd_, hdc);
        LayoutButtons();
        // Reload icons at the new DPI scale so they stay crisp.
        for (auto& e : appEntries_) e.icon = nullptr;
        for (auto& btn : pinnedButtons_) btn.icon = nullptr;
        appIconCache_.Clear();
        StartIconLoadThread();
        return 0;
    }

    case WM_DISPLAYCHANGE: {
        appBar_.Unregister();
        appBar_.Register(hwnd_, settings_.position, EffectiveThicknessPx());
        RECT rc = appBar_.GetReservedRect();
        SetWindowPos(hwnd_, HWND_TOPMOST,
                     rc.left, rc.top,
                     rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
        RECT client;
        GetClientRect(hwnd_, &client);
        HDC hdc = GetDC(hwnd_);
        renderer_.Resize(client.right, client.bottom, hdc);
        ReleaseDC(hwnd_, hdc);
        LayoutButtons();
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_SETTINGS:
            if (SettingsDialog::Show(hwnd_, settings_)) {
                App::Instance().PropagateSettings(settings_, this);
                ApplySettings(settings_);
            }
            return 0;
        case IDM_EXPORT_SETTINGS:
            if (ExportSettingsToFile(settings_))
                MessageBoxW(hwnd_,
                            L"Settings exported to winzoo-settings.json in the application folder.",
                            L"Export successful", MB_OK | MB_ICONINFORMATION);
            else
                MessageBoxW(hwnd_,
                            L"Failed to write winzoo-settings.json.\n"
                            L"Check that the application folder is writable.",
                            L"Export failed", MB_OK | MB_ICONERROR);
            return 0;
        case IDM_ABOUT:
            MessageBoxW(hwnd_,
                        L"Winzoo v0.1\n\nA lightweight taskbar replacement for Windows 10/11.",
                        L"About Winzoo",
                        MB_OK | MB_ICONINFORMATION);
            return 0;
        case IDM_REBUILD_ICON_CACHE:
            for (auto& e : appEntries_) e.icon = nullptr;
            for (auto& btn : pinnedButtons_) btn.icon = nullptr;
            appIconCache_.Clear();
            StartIconLoadThread();
            return 0;
        case IDM_RESTART:
            App::Instance().RequestRestart();
            return 0;
        case IDM_CLOSE_TASKBAR:
            PostQuitMessage(0);
            return 0;
        default: break;
        }
        break;

    case WM_DESTROY:
        shutdownPending_ = true;
        KillTimer(hwnd, kTimerActiveWindow);
        KillTimer(hwnd, kTimerAppScanFirst);
        KillTimer(hwnd, kTimerAppScan);
        KillTimer(hwnd, kTimerStatus);
        KillTimer(hwnd, kTimerTray);
        // Drain any pending thread-posted messages to prevent heap/icon leaks.
        {
            MSG pendingMsg;
            while (PeekMessageW(&pendingMsg, hwnd, WM_APP_SCAN_DONE, WM_APP_ICONS_DONE, PM_REMOVE))
                HandleMessage(hwnd, pendingMsg.message, pendingMsg.wParam, pendingMsg.lParam);
        }
        for (auto& e : trayIcons_) if (e.hIcon) { DestroyIcon(e.hIcon); e.hIcon = nullptr; }
        trayIcons_.clear();
        tracker_.Shutdown();
        appBar_.Unregister();
        PostQuitMessage(0);
        return 0;
    default: break;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}
