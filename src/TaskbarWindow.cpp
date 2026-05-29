#include "TaskbarWindow.h"
#include "Registry.h"
#include "resource.h"
#include <algorithm>
#include <windowsx.h>

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

bool TaskbarWindow::Create(HINSTANCE hInst, const Settings& settings)
{
    hInst_    = hInst;
    settings_ = settings;
    colors_   = GetThemeColors(settings.theme);
    dpi_      = GetWindowDpi(nullptr);

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

RECT TaskbarWindow::CalculateWindowRect() const
{
    // Get primary monitor work area for initial estimate
    RECT desktop = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &desktop, 0);

    // Full monitor
    HMONITOR hMon = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
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
    if (buttons.empty()) return;

    int count   = static_cast<int>(buttons.size());
    int pad     = Scale(2, dpi_);
    int tail    = Scale(20, dpi_);   // always-empty right-click zone at the trailing edge
    int maxBtnW = Scale(200, dpi_);
    int minBtnW = Scale(48, dpi_);

    bool isHoriz = (settings_.position != TaskbarPosition::Left &&
                    settings_.position != TaskbarPosition::Right);

    if (isHoriz) {
        int available = w - 2 * pad - tail;
        int btnW = std::min(maxBtnW,
                            std::max(minBtnW,
                                     (available - (count - 1) * pad) / count));
        int x = pad;
        for (auto& btn : buttons) {
            btn.rect = { x, pad, x + btnW, h - pad };
            x += btnW + pad;
        }
    } else {
        int btnH    = Scale(36, dpi_);
        int maxY    = h - tail;   // stop before the trailing reserved zone
        int y       = pad;
        for (auto& btn : buttons) {
            if (y + btnH > maxY) break;
            btn.rect = { pad, y, w - pad, y + btnH };
            y += btnH + pad;
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

    if (IsIconic(btn.hwnd))
        ShowWindow(btn.hwnd, SW_RESTORE);

    if (GetForegroundWindow() == btn.hwnd) {
        ShowWindow(btn.hwnd, SW_MINIMIZE);
        return;
    }

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

    const auto& btn = buttons[idx];
    bool isPinned = btn.isPinned;
    bool isRunning = btn.IsRunning();

    std::vector<MenuItem> items = {
        { L"Open new window",       IDM_OPEN_NEW_WINDOW, false, false, !isRunning || btn.exePath.empty() },
        { L"",                      0,                   true,  false, false },
        { isPinned ? L"Unpin from taskbar" : L"Pin to taskbar",
                                    IDM_PIN_UNPIN,       false, isPinned, false },
        { L"Close window",          IDM_CLOSE_WINDOW,    false, false, !isRunning },
    };

    UINT id = PopupMenu::Show(hwnd_, ptScreen, std::move(items), colors_, dpi_);

    switch (id) {
    case IDM_OPEN_NEW_WINDOW:
        if (!btn.exePath.empty())
            ShellExecuteW(nullptr, L"open", btn.exePath.c_str(),
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
        if (idx < static_cast<int>(tracker_.Buttons().size()) &&
            tracker_.Buttons()[idx].IsRunning())
            PostMessage(tracker_.Buttons()[idx].hwnd, WM_CLOSE, 0, 0);
        break;
    }
}

void TaskbarWindow::ShowBackgroundMenu(POINT ptScreen)
{
    std::vector<MenuItem> items = {
        { L"Settings",       IDM_SETTINGS,      false, false, false },
        { L"About Winzoo",   IDM_ABOUT,         false, false, false },
        { L"",               0,                 true,  false, false },
        { L"Close Taskbar",  IDM_CLOSE_TASKBAR, false, false, false },
    };

    UINT id = PopupMenu::Show(hwnd_, ptScreen, std::move(items), colors_, dpi_);

    switch (id) {
    case IDM_SETTINGS:
        if (SettingsDialog::Show(hwnd_, settings_))
            ApplySettings(settings_);
        break;

    case IDM_ABOUT:
        MessageBoxW(hwnd_,
                    L"Winzoo v0.1\n\nA lightweight taskbar replacement for Windows 10/11.",
                    L"About Winzoo",
                    MB_OK | MB_ICONINFORMATION);
        break;

    case IDM_CLOSE_TASKBAR:
        PostQuitMessage(0);
        break;
    }
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
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT client;
        GetClientRect(hwnd, &client);

        POINT ghostPt = drag_.CurrentPoint();
        ScreenToClient(hwnd, &ghostPt);

        renderer_.Paint(hdc, client.right, client.bottom,
                        tracker_.Buttons(),
                        hoveredIdx_,
                        drag_.State() == DragState::Pressed ? drag_.DragIndex() : -1,
                        drag_.State() == DragState::Dragging ? drag_.DragIndex() : -1,
                        ghostPt,
                        colors_, dpi_);
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
        hoveredIdx_ = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
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

    case WM_TIMER:
        if (wParam == kTimerActiveWindow) {
            tracker_.UpdateActiveWindow();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

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
        KillTimer(hwnd, kTimerActiveWindow);
        tracker_.Shutdown();
        appBar_.Unregister();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}
