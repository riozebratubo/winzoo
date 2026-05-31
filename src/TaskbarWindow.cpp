#include "TaskbarWindow.h"
#include "App.h"
#include "Registry.h"
#include "SettingsFile.h"
#include "resource.h"
#include <algorithm>
#include <atomic>
#include <windowsx.h>
#include <objbase.h>

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

static constexpr wchar_t kClassName[] = L"WinzooTaskbar";

bool TaskbarWindow::RegisterWndClass(HINSTANCE hInst)
{
    WNDCLASSEXW existing = { sizeof(existing) };
    if (GetClassInfoExW(hInst, kClassName, &existing)) return true;

    WNDCLASSEXW wc = { sizeof(wc) };
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
    showStartButton_ = isPrimary_
                     || settings_.showAppMenuOnAllMonitors
                     || settings_.taskbarMonitorMode == TaskbarMonitorMode::Primary;
}

RECT TaskbarWindow::CalculateWindowRect() const
{
    // Use the assigned monitor; fall back to primary if not set
    HMONITOR hMon = hMonitor_
                  ? hMonitor_
                  : MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfo(hMon, &mi);
    RECT mon = mi.rcMonitor;

    int thick = Scale(settings_.thickness, dpi_);

    switch (settings_.position) {
    case TaskbarPosition::Top:
        return { mon.left, mon.top, mon.right, mon.top + thick };
    case TaskbarPosition::Left:
        return { mon.left, mon.top, mon.left + thick, mon.bottom };
    case TaskbarPosition::Right:
        return { mon.right - thick, mon.top, mon.right, mon.bottom };
    case TaskbarPosition::Floating:
        return { settings_.floatX, settings_.floatY,
                 settings_.floatX + Scale(400, dpi_),
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
    settings_ = s;
    colors_   = GetThemeColors(s.theme);
    showStartButton_ = isPrimary_
                     || settings_.showAppMenuOnAllMonitors
                     || settings_.taskbarMonitorMode == TaskbarMonitorMode::Primary;
    SaveSettings(s);

    if (settings_.position != TaskbarPosition::Floating) {
        appBar_.SetPosition(settings_.position, Scale(settings_.thickness, dpi_));
        RECT rc = appBar_.GetReservedRect();
        SetWindowPos(hwnd_, HWND_TOPMOST,
                     rc.left, rc.top,
                     rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
        RECT client;
        GetClientRect(hwnd_, &client);
        renderer_.Resize(client.right, client.bottom,
                         GetDC(hwnd_));
    } else {
        appBar_.Unregister();
        RECT rc = CalculateWindowRect();
        SetWindowPos(hwnd_, HWND_TOPMOST,
                     rc.left, rc.top,
                     rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOACTIVATE);
    }

    LayoutButtons();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void TaskbarWindow::LayoutButtons()
{
    if (!hwnd_) return;

    RECT client;
    GetClientRect(hwnd_, &client);
    int w = client.right  - client.left;
    int h = client.bottom - client.top;

    auto& buttons = tracker_.MutableButtons();

    // Determine visible buttons. When filtering by current monitor, buttons whose
    // window is on a different monitor are hidden (rect zeroed). Pinned-only buttons
    // (no running HWND) appear only on the primary taskbar when filtering is active.
    bool filterByMonitor = settings_.showCurrentMonitorAppsOnly
                        && settings_.taskbarMonitorMode == TaskbarMonitorMode::AllMonitors
                        && hMonitor_ != nullptr;

    std::vector<TaskButton*> visible;
    visible.reserve(buttons.size());
    for (auto& btn : buttons) {
        bool show = true;
        if (filterByMonitor) {
            if (btn.hwnd)
                show = (MonitorFromWindow(btn.hwnd, MONITOR_DEFAULTTONULL) == hMonitor_);
            else
                show = isPrimary_;
        }
        if (show)
            visible.push_back(&btn);
        else
            btn.rect = {};
    }

    int count = static_cast<int>(visible.size());

    clockRect_       = {};
    scrollNeeded_    = false;
    scrollLeftRect_  = scrollRightRect_ = {};
    maxScrollOffset_ = 0;

    // Start button: a square on the leading edge (hidden if showStartButton_ is false)
    int startSz  = showStartButton_ ? Scale(settings_.thickness, dpi_) : 0;
    bool isHoriz = (settings_.position != TaskbarPosition::Left &&
                    settings_.position != TaskbarPosition::Right);
    int pad      = Scale(2, dpi_);

    if (showStartButton_) {
        if (isHoriz)
            startBtnRect_ = { 0, pad, startSz, h - pad };
        else
            startBtnRect_ = { pad, 0, w - pad, startSz };
    } else {
        startBtnRect_ = {};
    }

    if (count == 0) return;
    int arrowW  = Scale(20, dpi_);
    int maxBtnW = Scale(settings_.maxButtonWidth, dpi_);
    int minBtnW = Scale(settings_.minButtonWidth, dpi_);
    if (minBtnW > maxBtnW) minBtnW = maxBtnW;

    // Tail reservation
    int tail = 0;
    if (settings_.showClock) {
        int cw = Scale(settings_.clockWidth, dpi_);
        tail = cw;
        clockRect_ = isHoriz ? RECT{ w - cw, pad, w - pad, h - pad }
                              : RECT{ pad, h - cw, w - pad, h - pad };
    } else {
        tail = settings_.showRightClickGap ? Scale(20, dpi_) : 0;
    }

    if (isHoriz) {
        // Available area starts after start button
        int start     = startSz;
        int available = w - start - tail;

        int totalMin = count * minBtnW + (count - 1) * pad + 2 * pad;

        if (totalMin <= available) {
            scrollOffset_ = 0;
            int area = available - 2 * pad;
            int btnW = std::min(maxBtnW,
                                std::max(minBtnW,
                                         (area - (count - 1) * pad) / count));
            int x = start + pad;
            for (auto* btn : visible) {
                btn->rect = { x, pad, x + btnW, h - pad };
                x += btnW + pad;
            }
        } else {
            scrollNeeded_    = true;
            scrollLeftRect_  = { start,                      0, start + arrowW,    h };
            scrollRightRect_ = { start + available - arrowW, 0, start + available, h };

            int inner    = available - 2 * arrowW - 2 * pad;
            int visCount = std::max(1, (inner + pad) / (minBtnW + pad));
            maxScrollOffset_ = std::max(0, count - visCount);
            scrollOffset_    = std::min(scrollOffset_, maxScrollOffset_);

            for (auto* btn : visible) btn->rect = {};
            int x = start + arrowW + pad;
            for (int i = scrollOffset_; i < scrollOffset_ + visCount && i < count; ++i) {
                visible[i]->rect = { x, pad, x + minBtnW, h - pad };
                x += minBtnW + pad;
            }
        }
    } else {
        int start     = startSz;
        int btnH      = Scale(36, dpi_);
        int available = h - start - tail;
        int totalMin  = count * minBtnW + (count - 1) * pad + 2 * pad;

        if (totalMin <= available) {
            scrollOffset_ = 0;
            int y = start + pad;
            for (auto* btn : visible) {
                btn->rect = { pad, y, w - pad, y + btnH };
                y += btnH + pad;
            }
        } else {
            scrollNeeded_    = true;
            scrollLeftRect_  = { 0, start,                      w, start + arrowW };
            scrollRightRect_ = { 0, start + available - arrowW, w, start + available };

            int inner    = available - 2 * arrowW - 2 * pad;
            int visCount = std::max(1, (inner + pad) / (minBtnW + pad));
            maxScrollOffset_ = std::max(0, count - visCount);
            scrollOffset_    = std::min(scrollOffset_, maxScrollOffset_);

            for (auto* btn : visible) btn->rect = {};
            int y = start + arrowW + pad;
            for (int i = scrollOffset_; i < scrollOffset_ + visCount && i < count; ++i) {
                visible[i]->rect = { pad, y, w - pad, y + minBtnW };
                y += minBtnW + pad;
            }
        }
    }
}

int TaskbarWindow::HitTestButton(POINT pt) const
{
    const auto& buttons = tracker_.Buttons();
    for (int i = 0; i < static_cast<int>(buttons.size()); ++i) {
        if (buttons[i].HitTest(pt)) return i;
    }
    return -1;
}

void TaskbarWindow::ActivateButton(int idx)
{
    auto& buttons = tracker_.MutableButtons();
    if (idx < 0 || idx >= static_cast<int>(buttons.size())) return;

    TaskButton& btn = buttons[idx];

    if (!btn.IsRunning()) {
        if (!btn.exePath.empty())
            ShellExecuteW(nullptr, L"open", btn.exePath.c_str(),
                          nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }

    // Minimize only if the window is already visible and in the foreground.
    // Check this before any restore so a minimized window isn't immediately
    // re-minimized after SW_RESTORE makes it the foreground window.
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

void TaskbarWindow::ShowTaskButtonMenu(int idx, POINT ptScreen)
{
    const auto& buttons = tracker_.Buttons();
    if (idx < 0 || idx >= static_cast<int>(buttons.size())) return;

    // Copy values we need before the modal message loop can invalidate the reference.
    const std::wstring exePath  = buttons[idx].exePath;
    const HWND        btnHwnd  = buttons[idx].hwnd;
    bool isPinned  = buttons[idx].isPinned;
    bool isRunning = buttons[idx].IsRunning();

    std::vector<MenuItem> items = {
        { L"Open new window",       IDM_OPEN_NEW_WINDOW, false, false, exePath.empty() },
        { L"",                      0,                   true,  false, false },
        { isPinned ? L"Unpin from taskbar" : L"Pin to taskbar",
                                    IDM_PIN_UNPIN,       false, isPinned, false },
        { L"Close window",          IDM_CLOSE_WINDOW,    false, false, !isRunning },
    };

    UINT id = PopupMenu::Show(hwnd_, ptScreen, std::move(items), colors_, dpi_);

    switch (id) {
    case IDM_OPEN_NEW_WINDOW:
        if (!exePath.empty())
            ShellExecuteW(nullptr, L"open", exePath.c_str(),
                          nullptr, nullptr, SW_SHOWNORMAL);
        break;

    case IDM_PIN_UNPIN: {
        auto& mutableBtns = tracker_.MutableButtons();
        if (idx < static_cast<int>(mutableBtns.size())) {
            mutableBtns[idx].isPinned = !mutableBtns[idx].isPinned;
            // Sync pinned paths in settings
            Settings updated = settings_;
            updated.pinnedExePaths.clear();
            for (const auto& b : mutableBtns)
                if (b.isPinned && !b.exePath.empty())
                    updated.pinnedExePaths.push_back(b.exePath);
            ApplySettings(updated);
        }
        break;
    }

    case IDM_CLOSE_WINDOW:
        if (btnHwnd && isRunning)
            PostMessage(btnHwnd, WM_CLOSE, 0, 0);
        break;
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
    WPARAM wp = isFirstScan ? 0 : 1;
    std::thread([hwnd, wp]() {
        auto* pEntries = new std::vector<AppEntry>(AppScanner::Scan());
        PostMessageW(hwnd, WM_APP_SCAN_DONE, wp,
                     reinterpret_cast<LPARAM>(pEntries));
    }).detach();
}

void TaskbarWindow::StartIconLoadThread()
{
    HWND hwnd = hwnd_;
    int  sizePx = Scale(48, dpi_);
    std::vector<std::wstring> paths;
    paths.reserve(appEntries_.size());
    for (const auto& e : appEntries_) {
        paths.push_back(e.iconPath.empty() ? e.exePath : e.iconPath);
    }
    std::thread([hwnd, sizePx, paths = std::move(paths)]() {
        using Pair = std::pair<std::wstring, HICON>;
        auto* pResults = new std::vector<Pair>(paths.size());
        for (size_t i = 0; i < paths.size(); ++i)
            (*pResults)[i].first = paths[i];

        // Load icons in parallel: up to 4 workers each grab paths via an atomic counter.
        // MTA avoids STA message-pump blocking; each worker owns its COM apartment.
        const int kWorkers = std::min((int)paths.size(), 4);
        if (kWorkers > 0) {
            std::atomic<int> nextIdx{ 0 };
            std::vector<std::thread> workers;
            workers.reserve(kWorkers);
            for (int w = 0; w < kWorkers; ++w) {
                workers.emplace_back([&paths, pResults, &nextIdx, sizePx]() {
                    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    int idx;
                    while ((idx = nextIdx.fetch_add(1)) < (int)paths.size())
                        (*pResults)[idx].second = AppIconCache::LoadStatic(paths[idx], sizePx);
                    if (SUCCEEDED(hrCom)) CoUninitialize();
                });
            }
            for (auto& t : workers) t.join();
        }

        if (!PostMessageW(hwnd, WM_APP_ICONS_DONE, static_cast<WPARAM>(sizePx),
                          reinterpret_cast<LPARAM>(pResults))) {
            for (auto& [p, icon] : *pResults)
                if (icon) DestroyIcon(icon);
            delete pResults;
        }
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
    if (taskbarCreatedMsg_ && uMsg == taskbarCreatedMsg_) {
        HWND tray = FindWindow(L"Shell_TrayWnd", nullptr);
        if (tray) ShowWindow(tray, SW_HIDE);
        appBar_.Unregister();
        appBar_.Register(hwnd_, settings_.position, Scale(settings_.thickness, dpi_));
        RECT rc = appBar_.GetReservedRect();
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
            appBar_.Register(hwnd_, settings_.position,
                             Scale(settings_.thickness, dpi_));

        shellHookMsg_ = tracker_.ShellHookMessage();
        if (shellHookMsg_ == 0)
            shellHookMsg_ = RegisterWindowMessage(L"SHELLHOOK");
        tracker_.Initialize(hwnd_, &iconCache_,
                            [this]() {
                                LayoutButtons();
                                InvalidateRect(hwnd_, nullptr, FALSE);
                            });

        LayoutButtons();
        SetTimer(hwnd, kTimerActiveWindow, kTimerIntervalMs, nullptr);
        SetTimer(hwnd, kTimerAppScanFirst, 500, nullptr);
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
            clock.timeFontPt = settings_.clockTimeFontSize;
            clock.dateFontPt = settings_.clockDateFontSize;
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

        renderer_.Paint(hdc, client.right, client.bottom,
                        tracker_.Buttons(),
                        hoveredIdx_,
                        drag_.State() == DragState::Pressed  ? drag_.DragIndex() : -1,
                        drag_.State() == DragState::Dragging ? drag_.DragIndex() : -1,
                        ghostPt,
                        colors_, dpi_, clock, scroll,
                        StartButtonInfo{ showStartButton_, startBtnRect_, hoveredStart_ },
                        MinimizedIndicatorOptions{
                            settings_.showMinimizedIndicator,
                            settings_.minimizedIndicatorW,
                            settings_.minimizedIndicatorH
                        });
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

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

        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);

        if (drag_.State() != DragState::Idle) {
            POINT screenPt = pt;
            ClientToScreen(hwnd, &screenPt);
            drag_.OnMouseMove(screenPt);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        hoveredIdx_    = -1;
        hoveredScroll_ = 0;
        hoveredStart_  = false;
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

        if (wasDragging) {
            // Perform the drop: swap dragged button to hovered position
            int dropIdx = HitTestButton(pt);
            if (dropIdx >= 0 && dropIdx != origIdx) {
                auto& buttons = tracker_.MutableButtons();
                std::swap(buttons[origIdx], buttons[dropIdx]);
                LayoutButtons();
            }
        }

        drag_.OnButtonUp();
        ReleaseCapture();

        if (wasPressed) {
            ActivateButton(origIdx);
        }

        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_CAPTURECHANGED:
        drag_.OnCaptureChanged();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_MBUTTONUP: {
        if (settings_.middleClickClose) {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            int idx = HitTestButton(pt);
            if (idx >= 0 && idx < static_cast<int>(tracker_.Buttons().size())) {
                HWND target = tracker_.Buttons()[idx].hwnd;
                if (target) PostMessage(target, WM_CLOSE, 0, 0);
            }
        }
        return 0;
    }

    case WM_RBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        POINT screenPt = pt;
        ClientToScreen(hwnd, &screenPt);
        int idx = HitTestButton(pt);
        if (idx >= 0)
            ShowTaskButtonMenu(idx, screenPt);
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
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wParam == kTimerAppScanFirst) {
            KillTimer(hwnd, kTimerAppScanFirst);
            StartScanThread(true);
        } else if (wParam == kTimerAppScan) {
            StartScanThread(false);
        }
        return 0;

    case WM_APP_SCAN_DONE: {
        auto* pEntries = reinterpret_cast<std::vector<AppEntry>*>(lParam);
        if (!shutdownPending_) {
            int iconSz = Scale(48, dpi_);
            if (wParam == 0) {
                // First scan: icons not yet cached; leave them null until icon thread finishes.
                appEntries_ = std::move(*pEntries);
                SetTimer(hwnd_, kTimerAppScan, kTimerAppScanMs, nullptr);
                StartIconLoadThread();
            } else {
                // Periodic re-scan: assign whatever is already in cache (no I/O).
                for (auto& e : *pEntries) {
                    const std::wstring& p = e.iconPath.empty() ? e.exePath : e.iconPath;
                    e.icon = appIconCache_.TryGet(p, iconSz);
                }
                appEntries_ = std::move(*pEntries);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        delete pEntries;
        return 0;
    }

    case WM_APP_ICONS_DONE: {
        auto* pIcons = reinterpret_cast<std::vector<std::pair<std::wstring, HICON>>*>(lParam);
        if (!shutdownPending_) {
            int iconSz = static_cast<int>(wParam);
            for (auto& [path, icon] : *pIcons)
                appIconCache_.Store(path, iconSz, icon);
            // Only bind icons to entries if they match the current desired size,
            // preventing a stale load thread (e.g. from before a DPI change) from
            // assigning wrong-sized icons to the live entry list.
            if (iconSz == Scale(48, dpi_)) {
                for (auto& e : appEntries_) {
                    const std::wstring& p = e.iconPath.empty() ? e.exePath : e.iconPath;
                    e.icon = appIconCache_.TryGet(p, iconSz);
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        } else {
            // App is shutting down — free HICON resources that won't be stored.
            for (auto& [path, icon] : *pIcons)
                if (icon) DestroyIcon(icon);
        }
        delete pIcons;
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
        appIconCache_.Clear();
        StartIconLoadThread();
        return 0;
    }

    case WM_DISPLAYCHANGE: {
        appBar_.Unregister();
        appBar_.Register(hwnd_, settings_.position,
                         Scale(settings_.thickness, dpi_));
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

    case WM_DESTROY:
        shutdownPending_ = true;
        KillTimer(hwnd, kTimerActiveWindow);
        KillTimer(hwnd, kTimerAppScanFirst);
        KillTimer(hwnd, kTimerAppScan);
        tracker_.Shutdown();
        appBar_.Unregister();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}
